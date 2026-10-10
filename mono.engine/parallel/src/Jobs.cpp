#include "ThreadAffinity.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace engine::parallel {

	const char *Describe(JobContext context) {
		switch (context) {
		case JobContext::Serial:
			return "Serial";
		case JobContext::Threaded:
			return "Threaded";
		case JobContext::Processed:
			return "Processed";
		}
		return "Unknown";
	}

	namespace {
		struct Pool;

		struct AssignedReading {
			uint64_t InclusiveNanoseconds = 0;
			uint64_t BusyNanoseconds = 0;
			bool Entered = false;
			bool TookTask = false;
		};

		struct WorkerWake {
			std::condition_variable Available;
			uint64_t Generation = 0;
		};

		// One batch may use the pool; competing dispatches run inline.
		struct Batch {
			Pool *Owner = nullptr;
			const std::function<void(size_t, size_t)> *Body = nullptr;
			const unsigned *AssignedWorkers = nullptr;
			bool CaptureAssigned = false;
			size_t Count = 0;
			size_t Grain = 0;
			std::atomic<size_t> Next{0};

			// Count ranges, not workers, so the join tracks completed work.
			std::atomic<size_t> Outstanding{0};

			std::mutex FailureGuard;
			std::exception_ptr Failure;

			// Aggregated before the final range releases the join.
			std::atomic<uint64_t> BusyNanoseconds{0};
			std::atomic<uint32_t> Participants{0};
		};

		thread_local BatchTiming LastTiming;

		// Process-wide so profiling cannot be bypassed by worker threads.
		std::atomic<bool> Forced{false};

		BatchTiming Inline(uint64_t nanoseconds) {
			const auto milliseconds = static_cast<float>(static_cast<double>(nanoseconds) / 1e6);
			return BatchTiming{milliseconds, milliseconds, 1};
		}

		struct Pool {
			Pool() {
				Slot.Owner = this;
			}
			~Pool();

			std::vector<std::thread> Workers;
			std::vector<platform::Processor> WorkerProcessors;
			std::vector<uint8_t> WorkerPinned;
			std::vector<AssignedReading> AssignedReadings;
			std::vector<std::unique_ptr<WorkerWake>> WorkerWakes;
			std::vector<unsigned> SignaledWorkers;
			std::mutex Guard;
			std::condition_variable Finished;
			std::condition_variable ReadyCondition;

			// The slot cannot be reused while a worker still holds it.
			std::condition_variable Drained;

			// Pool-owned because workers may outlive the range-counted join.
			Batch Slot;

			// Workers that read Current before leaving Drain.
			size_t Inside = 0;

			// Claiming prevents concurrent callers from sharing the slot.
			std::atomic<bool> Claimed{false};

			Batch *Current = nullptr;
			uint64_t Generation = 0;
			unsigned Ready = 0;
			unsigned PinnedWorkers = 0;
			bool Stopping = false;
		};

		struct PoolStorage {
			std::mutex Guard;
			std::unique_ptr<Pool> Active;
		};

		PoolStorage &Storage() {
			static PoolStorage storage;
			return storage;
		}

		Pool &CreatePool() {
			PoolStorage &storage = Storage();
			std::lock_guard lock(storage.Guard);
			if (storage.Active == nullptr) {
				storage.Active = std::make_unique<Pool>();
			}
			return *storage.Active;
		}

		Pool *ExistingPool() {
			PoolStorage &storage = Storage();
			std::lock_guard lock(storage.Guard);
			return storage.Active.get();
		}

		constexpr size_t NO_RANGE = static_cast<size_t>(-1);

		void RunBody(Batch &batch, size_t begin, uint64_t &busy) {
			const size_t end = std::min(begin + batch.Grain, batch.Count);

			const uint64_t started = core::Clock::Nanoseconds();
			try {
				(*batch.Body)(begin, end);
			} catch (...) {
				std::lock_guard lock(batch.FailureGuard);
				if (!batch.Failure) {
					batch.Failure = std::current_exception();
				}
			}
			busy += core::Clock::Nanoseconds() - started;
		}

		// A failed range still retires so the join cannot hang.
		void Retire(Batch &batch) {
			// Hold the guard while notifying to avoid a lost wakeup.
			if (batch.Outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				Pool &pool = *batch.Owner;
				std::lock_guard lock(pool.Guard);
				pool.Finished.notify_all();
			}
		}

		// Hold one range until participant totals are published.
		void Drain(Batch &batch, size_t first = NO_RANGE) {
			uint64_t busy = 0;
			bool took = false;
			size_t pending = NO_RANGE;
			size_t begin = first;

			for (;;) {
				if (begin == NO_RANGE) {
					begin = batch.Next.fetch_add(batch.Grain, std::memory_order_relaxed);
					if (begin >= batch.Count) {
						break;
					}
				}

				if (pending != NO_RANGE) {
					Retire(batch);
				}

				RunBody(batch, begin, busy);
				took = true;
				pending = begin;
				begin = NO_RANGE;
			}

			if (took) {
				batch.BusyNanoseconds.fetch_add(busy, std::memory_order_relaxed);
				batch.Participants.fetch_add(1, std::memory_order_relaxed);
			}

			if (pending != NO_RANGE) {
				Retire(batch);
			}
		}

		void DrainAssigned(Batch &batch, unsigned workerIndex) {
			const uint64_t started = batch.CaptureAssigned ? core::Clock::Nanoseconds() : 0;
			uint64_t busy = 0;
			bool took = false;
			bool pending = false;

			for (size_t index = 0; index < batch.Count; index++) {
				if (batch.AssignedWorkers[index] != workerIndex) {
					continue;
				}

				// Keep one completion private until this worker has published its
				// timing. Otherwise the caller can observe the join before the last
				// worker contributes to the batch totals.
				if (pending) {
					Retire(batch);
				}
				RunBody(batch, index, busy);
				took = true;
				pending = true;
			}

			if (took) {
				batch.BusyNanoseconds.fetch_add(busy, std::memory_order_relaxed);
				batch.Participants.fetch_add(1, std::memory_order_relaxed);
			}

			if (pending) {
				Retire(batch);
			}
			if (batch.CaptureAssigned) {
				// Published before Inside retires, including workers that found no assigned task.
				batch.Owner->AssignedReadings[workerIndex] = {
					core::Clock::Nanoseconds() - started, busy, true, took
				};
			}
		}

		void WorkerLoop(Pool &pool, unsigned workerIndex) {
			// **The whole thread, not each batch.** A heap tag is per thread, so
			// opening one here means everything a worker ever allocates is
			// attributed to the pool rather than landing in the untagged pile
			// beside the process's static initialisers - which is where a leak
			// on a job thread would otherwise be invisible.
			ENGINE_HEAP_SCOPE("jobs.worker");

			uint64_t seen = 0;
			WorkerWake &wake = *pool.WorkerWakes[workerIndex];

			const bool pinned = platform::PinCurrentThread(pool.WorkerProcessors[workerIndex]);
			{
				std::lock_guard lock(pool.Guard);
				pool.WorkerPinned[workerIndex] = pinned ? 1 : 0;
				pool.Ready++;
				pool.ReadyCondition.notify_one();
			}

			for (;;) {
				Batch *batch = nullptr;
				{
					std::unique_lock lock(pool.Guard);
					wake.Available.wait(lock, [&] { return pool.Stopping || wake.Generation != seen; });
					if (pool.Stopping) {
						return;
					}
					seen = wake.Generation;
					// A generic batch may finish before a signaled worker wakes. Its
					// stale notification must not enter a later, unassigned batch.
					batch = seen == pool.Generation ? pool.Current : nullptr;

					// Count the worker while holding the lock that protects Current.
					if (batch != nullptr) {
						pool.Inside++;
					}
				}

				if (batch != nullptr) {
					if (batch->AssignedWorkers == nullptr) {
						Drain(*batch);
					} else {
						DrainAssigned(*batch, workerIndex);
					}

					std::lock_guard lock(pool.Guard);
					if (--pool.Inside == 0) {
						pool.Drained.notify_all();
					}
				}
			}
		}

		void ShutdownPool(Pool &pool) {
			{
				std::lock_guard lock(pool.Guard);
				if (pool.Workers.empty()) {
					return;
				}
				pool.Stopping = true;
			}
			for (const auto &wake : pool.WorkerWakes)
				wake->Available.notify_one();

			for (std::thread &worker : pool.Workers) {
				worker.join();
			}

			std::lock_guard lock(pool.Guard);
			pool.Workers.clear();
			pool.WorkerProcessors.clear();
			pool.WorkerPinned.clear();
			pool.WorkerWakes.clear();
			pool.SignaledWorkers.clear();
			pool.Ready = 0;
			pool.PinnedWorkers = 0;
		}

		Pool::~Pool() {
			ShutdownPool(*this);
		}
	}

	void Jobs::Start(unsigned workers) {
		auto &pool = CreatePool();
		std::unique_lock lock(pool.Guard);

		if (!pool.Workers.empty()) {
			return;
		}

		const std::vector<platform::Processor> available = platform::AvailableProcessors();
		const std::vector<platform::Processor> distinctCores = platform::DistinctCoreProcessors();
		if (workers == 0) {
			// Leave one logical processor for the participating caller.
			workers = available.size() > 1 ? static_cast<unsigned>(available.size() - 1) : 0;
		}

		pool.Stopping = false;
		pool.Ready = 0;
		pool.PinnedWorkers = 0;
		pool.Workers.reserve(workers);
		pool.WorkerProcessors.resize(workers);
		pool.WorkerPinned.assign(workers, 0);
		{
			ENGINE_HEAP_SCOPE("jobs.worker.wakes");
			pool.WorkerWakes.reserve(workers);
			pool.SignaledWorkers.reserve(workers);
			for (unsigned index = 0; index < workers; ++index)
				pool.WorkerWakes.push_back(std::make_unique<WorkerWake>());
		}
		{
			ENGINE_HEAP_SCOPE("jobs.assigned.readings");
			pool.AssignedReadings.resize(workers);
		}
		for (unsigned index = 0; index < workers && index < distinctCores.size(); index++) {
			pool.WorkerProcessors[index] = distinctCores[index];
		}
		for (unsigned index = 0; index < workers; index++) {
			pool.Workers.emplace_back(WorkerLoop, std::ref(pool), index);
		}

		pool.ReadyCondition.wait(lock, [&] { return pool.Ready == workers; });
		while (pool.PinnedWorkers < pool.WorkerPinned.size() && pool.WorkerPinned[pool.PinnedWorkers] != 0) {
			pool.PinnedWorkers++;
		}

		const size_t expectedPinned = std::min<size_t>(workers, distinctCores.size());
		if (workers > 0 && distinctCores.empty()) {
			ENGINE_WARN(
				"job system could not identify bindable physical cores; assigned-worker dispatch will run "
				"inline"
			);
		} else if (pool.PinnedWorkers < expectedPinned) {
			ENGINE_WARN(
				"job system pinned only {} of {} physical-core worker(s); assigned-worker dispatch will use "
				"that prefix",
				pool.PinnedWorkers,
				expectedPinned
			);
		}

		ENGINE_INFO(
			"job system started with {} worker(s), {} physical-core pinned", workers, pool.PinnedWorkers
		);
	}

	void Jobs::Stop() {
		PoolStorage &storage = Storage();
		std::unique_ptr<Pool> stopped;
		{
			std::lock_guard lock(storage.Guard);
			stopped = std::move(storage.Active);
		}
		// Destruction joins every worker before releasing the condition variables.
		stopped.reset();
	}

	unsigned Jobs::WorkerCount() {
		Pool *pool = ExistingPool();
		if (pool == nullptr) {
			return 0;
		}
		std::lock_guard lock(pool->Guard);
		return static_cast<unsigned>(pool->Workers.size());
	}

	unsigned Jobs::PinnedWorkerCount() {
		Pool *pool = ExistingPool();
		if (pool == nullptr) {
			return 0;
		}
		std::lock_guard lock(pool->Guard);
		return pool->PinnedWorkers;
	}

	void
	Jobs::For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body, size_t minimum) {
		if (count == 0) {
			return;
		}
		if (grain == 0) {
			grain = DEFAULT_GRAIN;
		}

		if (Forced.load(std::memory_order_relaxed)) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		Pool *existing = ExistingPool();
		if (existing == nullptr) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}
		Pool &pool = *existing;

		size_t workers = 0;
		{
			std::lock_guard lock(pool.Guard);
			workers = pool.Workers.size();
		}
		// The default floor was measured against cheap per-index work.
		const size_t floor = minimum > 0 ? minimum : grain * Jobs::MINIMUM_GRAINS;
		if (workers == 0 || count < floor) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		// A competing or nested dispatch runs inline instead of sharing state.
		if (pool.Claimed.exchange(true, std::memory_order_acquire)) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		struct ClaimGuard {
			Pool &Owner;
			~ClaimGuard() {
				Owner.Claimed.store(false, std::memory_order_release);
			}
		} claim{pool};

		const uint64_t dispatched = core::Clock::Nanoseconds();

		const size_t ranges = (count + grain - 1) / grain;
		const size_t signaled = std::min(workers, ranges - 1);

		Batch &batch = pool.Slot;

		{
			std::unique_lock lock(pool.Guard);

			// Do not rewrite the slot while a prior worker still holds it.
			pool.Drained.wait(lock, [&] { return pool.Inside == 0; });

			batch.Body = &body;
			batch.AssignedWorkers = nullptr;
			batch.Count = count;
			batch.Grain = grain;
			batch.Outstanding.store(ranges, std::memory_order_relaxed);
			batch.BusyNanoseconds.store(0, std::memory_order_relaxed);
			batch.Participants.store(0, std::memory_order_relaxed);
			batch.Failure = nullptr;

			// Reserve the caller's range before waking workers.
			batch.Next.store(grain, std::memory_order_relaxed);

			pool.Current = &batch;
			pool.Generation++;
			pool.SignaledWorkers.clear();
			for (unsigned worker = 0; worker < signaled; ++worker) {
				pool.WorkerWakes[worker]->Generation = pool.Generation;
				pool.SignaledWorkers.push_back(worker);
			}
		}

		// Wake no more workers than the remaining ranges require.
		for (const unsigned worker : pool.SignaledWorkers)
			pool.WorkerWakes[worker]->Available.notify_one();
		if (core::FrameGraph::IsEnabled())
			core::Metrics::Count("jobs.signaled_workers", pool.SignaledWorkers.size());

		{
			ENGINE_PROFILE_CAT("jobs.drain", core::ProfileCategory::Engine);

			Drain(batch, 0);
		}

		{
			// Waiting for the last range is idle time, not dispatch work.
			ENGINE_PROFILE_CAT("jobs.join", core::ProfileCategory::Idle);
			std::unique_lock lock(pool.Guard);
			pool.Finished.wait(lock, [&] { return batch.Outstanding.load(std::memory_order_acquire) == 0; });

			pool.Current = nullptr;
		}

		LastTiming.BusyMilliseconds = static_cast<float>(
			static_cast<double>(batch.BusyNanoseconds.load(std::memory_order_relaxed)) / 1e6
		);
		LastTiming.WallMilliseconds =
			static_cast<float>(static_cast<double>(core::Clock::Nanoseconds() - dispatched) / 1e6);
		LastTiming.Participants = batch.Participants.load(std::memory_order_relaxed);

		if (batch.Failure) {
			std::rethrow_exception(batch.Failure);
		}
	}

	void Jobs::For(
		JobContext context,
		size_t count,
		size_t grain,
		const std::function<void(size_t, size_t)> &body,
		size_t minimum
	) {
		if (context == JobContext::Processed) {
			throw std::invalid_argument("Jobs::For cannot send a callback to another process");
		}
		if (context == JobContext::Threaded) {
			For(count, grain, body, minimum);
			return;
		}
		if (count == 0) {
			LastTiming = {};
			return;
		}

		const uint64_t started = core::Clock::Nanoseconds();
		body(0, count);
		LastTiming = Inline(core::Clock::Nanoseconds() - started);
	}

	void Jobs::ForWorkers(
		std::span<const unsigned> workerByIndex, const std::function<void(size_t, size_t)> &body
	) {
		ENGINE_PROFILE("jobs.assigned");
		if (workerByIndex.empty()) {
			return;
		}

		Pool *existing = ExistingPool();
		if (existing == nullptr) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, workerByIndex.size());
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}
		Pool &pool = *existing;
		unsigned pinnedWorkers = 0;
		{
			std::lock_guard lock(pool.Guard);
			pinnedWorkers = pool.PinnedWorkers;
		}

		const bool validMapping =
			pinnedWorkers > 0 &&
			std::all_of(workerByIndex.begin(), workerByIndex.end(), [pinnedWorkers](unsigned worker) {
				return worker < pinnedWorkers;
			});
		if (Forced.load(std::memory_order_relaxed) || !validMapping) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, workerByIndex.size());
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		if (pool.Claimed.exchange(true, std::memory_order_acquire)) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, workerByIndex.size());
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		struct ClaimGuard {
			Pool &Owner;
			~ClaimGuard() {
				Owner.Claimed.store(false, std::memory_order_release);
			}
		} claim{pool};

		const uint64_t dispatched = core::Clock::Nanoseconds();
		Batch &batch = pool.Slot;

		{
			std::unique_lock lock(pool.Guard);
			pool.Drained.wait(lock, [&] { return pool.Inside == 0; });
			batch.CaptureAssigned = core::FrameGraph::IsEnabled();
			if (batch.CaptureAssigned) {
				std::fill(pool.AssignedReadings.begin(), pool.AssignedReadings.end(), AssignedReading{});
			}

			batch.Body = &body;
			batch.AssignedWorkers = workerByIndex.data();
			batch.Count = workerByIndex.size();
			batch.Grain = 1;
			batch.Outstanding.store(workerByIndex.size(), std::memory_order_relaxed);
			batch.BusyNanoseconds.store(0, std::memory_order_relaxed);
			batch.Participants.store(0, std::memory_order_relaxed);
			batch.Failure = nullptr;

			pool.Current = &batch;
			pool.Generation++;
			pool.SignaledWorkers.clear();
			for (const unsigned worker : workerByIndex) {
				WorkerWake &wake = *pool.WorkerWakes[worker];
				if (wake.Generation == pool.Generation) continue;
				wake.Generation = pool.Generation;
				pool.SignaledWorkers.push_back(worker);
			}
		}

		for (const unsigned worker : pool.SignaledWorkers)
			pool.WorkerWakes[worker]->Available.notify_one();
		if (batch.CaptureAssigned) core::Metrics::Count("jobs.signaled_workers", pool.SignaledWorkers.size());

		{
			ENGINE_PROFILE_CAT("jobs.join.assigned", core::ProfileCategory::Idle);
			std::unique_lock lock(pool.Guard);
			pool.Finished.wait(lock, [&] { return batch.Outstanding.load(std::memory_order_acquire) == 0; });

			// Clear the shared pointer before waiting for workers that woke after
			// the last task finished. They then observe an empty batch rather than
			// a mapping whose caller is about to return.
			pool.Current = nullptr;
			pool.Drained.wait(lock, [&] { return pool.Inside == 0; });
		}

		batch.AssignedWorkers = nullptr;
		LastTiming.BusyMilliseconds = static_cast<float>(
			static_cast<double>(batch.BusyNanoseconds.load(std::memory_order_relaxed)) / 1e6
		);
		LastTiming.WallMilliseconds =
			static_cast<float>(static_cast<double>(core::Clock::Nanoseconds() - dispatched) / 1e6);
		LastTiming.Participants = batch.Participants.load(std::memory_order_relaxed);

		if (batch.CaptureAssigned) {
			uint64_t emptyNanoseconds = 0;
			unsigned emptyWorkers = 0;
			for (const AssignedReading &reading : pool.AssignedReadings) {
				if (!reading.Entered) continue;
				if (!reading.TookTask) {
					emptyNanoseconds += reading.InclusiveNanoseconds;
					emptyWorkers++;
					continue;
				}
				// Producer self time includes scanning, retirement and timer overhead, not wake/lock time.
				core::FrameGraph::ReportedScope worker(
					"jobs.assigned.worker",
					core::ProfileCategory::Engine,
					static_cast<float>(static_cast<double>(reading.InclusiveNanoseconds) / 1e6)
				);
				core::FrameGraph::Report(
					"jobs.assigned.body",
					core::ProfileCategory::Engine,
					static_cast<float>(static_cast<double>(reading.BusyNanoseconds) / 1e6)
				);
			}
			// Preserve observed empty-worker scan overhead without per-worker trees.
			core::Metrics::Count("jobs.assigned.empty_workers", emptyWorkers);
			if (emptyWorkers > 0) {
				core::FrameGraph::Report(
					"jobs.assigned.empty workers",
					core::ProfileCategory::Engine,
					static_cast<float>(static_cast<double>(emptyNanoseconds) / 1e6)
				);
			}
		}
		if (batch.Failure) {
			std::rethrow_exception(batch.Failure);
		}
	}

	BatchTiming Jobs::LastBatch() {
		return LastTiming;
	}

	void SetForceSerialCompute(bool forced) {
		Forced.store(forced, std::memory_order_relaxed);
	}

	bool ForceSerialCompute() {
		return Forced.load(std::memory_order_relaxed);
	}
}
