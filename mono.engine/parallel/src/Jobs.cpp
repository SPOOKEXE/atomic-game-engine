#include "ThreadAffinity.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace engine::parallel {

	namespace {

		// One batch may use the pool; competing dispatches run inline.
		struct Batch {
			const std::function<void(size_t, size_t)> *Body = nullptr;
			const unsigned *AssignedWorkers = nullptr;
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
			std::vector<std::thread> Workers;
			std::vector<platform::Processor> WorkerProcessors;
			std::vector<uint8_t> WorkerPinned;
			std::mutex Guard;
			std::condition_variable Available;
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

		// Never destroyed, deliberately, the same way `ecs::Components` and
		// `ecs::ChunkPool` are and for a problem of the same shape.
		//
		// Destroying the pool destroys four condition variables with every
		// worker still parked on `Available`, and `pthread_cond_destroy` waits
		// for the last waiter - so a process that called `Jobs::Start` and did
		// not call `Jobs::Stop` returned zero from `main` and then hung in
		// `exit` for ever. It reads as a hung test suite rather than as a stuck
		// teardown, which is what made it expensive to find. `Jobs::Stop` still
		// joins whoever asks for it; the process reclaims the memory.
		Pool &Get() {
			static Pool *pool = new Pool();
			return *pool;
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
				Pool &pool = Get();
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
		}

		void WorkerLoop(unsigned workerIndex) {
			// **The whole thread, not each batch.** A heap tag is per thread, so
			// opening one here means everything a worker ever allocates is
			// attributed to the pool rather than landing in the untagged pile
			// beside the process's static initialisers - which is where a leak
			// on a job thread would otherwise be invisible.
			ENGINE_HEAP_SCOPE("jobs.worker");

			auto &pool = Get();
			uint64_t seen = 0;

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
					pool.Available.wait(lock, [&] { return pool.Stopping || pool.Generation != seen; });
					if (pool.Stopping) {
						return;
					}
					seen = pool.Generation;
					batch = pool.Current;

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
	}

	void Jobs::Start(unsigned workers) {
		auto &pool = Get();
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
		for (unsigned index = 0; index < workers && index < distinctCores.size(); index++) {
			pool.WorkerProcessors[index] = distinctCores[index];
		}
		for (unsigned index = 0; index < workers; index++) {
			pool.Workers.emplace_back(WorkerLoop, index);
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
		auto &pool = Get();
		{
			std::lock_guard lock(pool.Guard);
			if (pool.Workers.empty()) {
				return;
			}
			pool.Stopping = true;
		}
		pool.Available.notify_all();

		// Outside the guard, and it has to be: a worker needs that mutex to
		// observe `Stopping`, so joining while holding it deadlocks.
		for (auto &worker : pool.Workers) {
			worker.join();
		}

		// Back under it for the tail. Every worker is joined by now so nothing
		// contends, but these five are read under the guard by `WorkerCount`,
		// `PinnedWorkerCount` and `For`, and one uncontended acquisition is
		// cheaper than an exception to the rule that they always are. It does
		// **not** make `Stop` thread-safe: two of them still join the same
		// threads, which is undefined whatever this holds.
		std::lock_guard lock(pool.Guard);
		pool.Workers.clear();
		pool.WorkerProcessors.clear();
		pool.WorkerPinned.clear();
		pool.Ready = 0;
		pool.PinnedWorkers = 0;
	}

	unsigned Jobs::WorkerCount() {
		auto &pool = Get();
		std::lock_guard lock(pool.Guard);
		return static_cast<unsigned>(pool.Workers.size());
	}

	unsigned Jobs::PinnedWorkerCount() {
		auto &pool = Get();
		std::lock_guard lock(pool.Guard);
		return pool.PinnedWorkers;
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

		auto &pool = Get();

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
		}

		// Wake no more workers than the remaining ranges require.
		if (ranges > workers) {
			pool.Available.notify_all();
		} else {
			for (size_t woken = 1; woken < ranges; woken++) {
				pool.Available.notify_one();
			}
		}

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

	void Jobs::ForWorkers(
		std::span<const unsigned> workerByIndex, const std::function<void(size_t, size_t)> &body
	) {
		if (workerByIndex.empty()) {
			return;
		}

		auto &pool = Get();
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
		}

		pool.Available.notify_all();

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
