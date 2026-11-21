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

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

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

		// One worker's private wake word, on its own cache line so a wake
		// touches nothing another worker is spinning on.
		//
		// `Asleep` and `Wake` are accessed sequentially consistently on both
		// sides, which makes the check a Dekker handshake: either the worker
		// sees its new `Wake` before it blocks, or the waker sees it asleep and
		// rings its band.
		struct alignas(64) WorkerSlot {
			std::atomic<uint32_t> Wake{0};
			std::atomic<bool> Asleep{false};
		};

		// Where a band of workers sleeps. Waking sleepers one futex call at a
		// time costs the dispatching thread a syscall per worker, which over a
		// full pool was several times the whole handover; one ring per band
		// wakes them together. A ring also wakes the band's workers that have
		// no task, and they go straight back to sleep.
		struct alignas(64) SleepBand {
			std::atomic<uint32_t> Bell{0};
		};

		constexpr unsigned BAND_WIDTH = 8;

		// Workers pinned to distinct physical cores spin on their wake word for
		// this long before sleeping. A frame runs its parallel loops back to
		// back, and a spinning worker takes the next batch in well under a
		// microsecond where a sleeping one costs a futex wake and, on a busy
		// machine, a wait for the scheduler. Only the pinned prefix spins: it is
		// what narrow dispatches and every `ForWorkers` lane use, and a whole
		// pool spinning across hyperthread siblings burns its scheduler share
		// and is then preempted holding a range.
		constexpr uint64_t SPIN_NANOSECONDS = 30'000;

		void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
			_mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
			__asm__ __volatile__("yield");
#endif
		}

		// Wait for `slot.Wake` to move off `seen`, spinning first when this
		// worker is a spinner, and return it.
		uint32_t AwaitWake(WorkerSlot &slot, SleepBand &band, uint32_t seen, bool spins) {
			uint32_t wake = slot.Wake.load(std::memory_order_acquire);
			if (wake != seen) {
				return wake;
			}

			if (spins) {
				const uint64_t until = core::Clock::Nanoseconds() + SPIN_NANOSECONDS;
				for (unsigned spin = 1;; spin++) {
					CpuRelax();
					wake = slot.Wake.load(std::memory_order_acquire);
					if (wake != seen) {
						return wake;
					}
					if ((spin & 63) == 0 && core::Clock::Nanoseconds() >= until) {
						break;
					}
				}
			}

			for (;;) {
				const uint32_t bell = band.Bell.load(std::memory_order_seq_cst);
				slot.Asleep.store(true, std::memory_order_seq_cst);
				wake = slot.Wake.load(std::memory_order_seq_cst);
				if (wake == seen) {
					band.Bell.wait(bell, std::memory_order_seq_cst);
					wake = slot.Wake.load(std::memory_order_acquire);
				}
				slot.Asleep.store(false, std::memory_order_relaxed);
				if (wake != seen) {
					return wake;
				}
			}
		}

		// Ring every band that `touched` marks.
		void Ring(SleepBand *bands, const std::vector<uint8_t> &touched) {
			for (size_t index = 0; index < touched.size(); index++) {
				if (touched[index] != 0) {
					bands[index].Bell.fetch_add(1, std::memory_order_seq_cst);
					bands[index].Bell.notify_all();
				}
			}
		}

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

			// Guards startup, shutdown and the worker-count queries. The
			// dispatch path never takes it: a batch is handed over through
			// each worker's `WorkerSlot` and joined on atomics.
			std::mutex Guard;
			std::condition_variable ReadyCondition;

			std::unique_ptr<WorkerSlot[]> Slots;
			std::unique_ptr<SleepBand[]> Bands;

			// Which bands the batch being published woke a worker in.
			std::vector<uint8_t> Touched;

			// Identity order for `For`, and the distinct named workers for
			// `ForWorkers`. Written only by the thread holding `Claimed`.
			std::vector<unsigned> Identity;
			std::vector<unsigned> Named;
			std::vector<uint8_t> NamedSeen;

			// Pool-owned because workers may outlive the range-counted join.
			Batch Slot;

			// Workers woken for the current batch that have not yet left it.
			// Set by the dispatcher before the first wake, so the slot cannot
			// be rewritten while any woken worker still reads it.
			std::atomic<uint32_t> Inside{0};

			// Claiming prevents concurrent callers from sharing the slot.
			std::atomic<bool> Claimed{false};

			// Read without the guard on dispatch; written only by Start and Stop,
			// which must not race a dispatch.
			std::atomic<unsigned> Size{0};

			// The pinned prefix, which spins before sleeping. Zero until Start
			// has counted it.
			std::atomic<unsigned> Spinners{0};

			Batch *Current = nullptr;
			uint32_t Generation = 0;
			unsigned Ready = 0;
			unsigned PinnedWorkers = 0;
			std::atomic<bool> Stopping{false};
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
			if (batch.Outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				batch.Outstanding.notify_all();
			}
		}

		// Block until `value` reads zero. The atomic wait spins briefly before
		// it sleeps, which is what a short join wants.
		template <class T>
		void WaitZero(const std::atomic<T> &value) {
			for (T seen = value.load(std::memory_order_acquire); seen != 0;
				 seen = value.load(std::memory_order_acquire)) {
				value.wait(seen, std::memory_order_acquire);
			}
		}

		// Publish a filled batch and wake the named workers. The dispatching
		// thread signals each one directly: a spinning worker costs a store,
		// and a chain of workers waking each other stalls behind whichever
		// link the scheduler preempts.
		void Publish(Pool &pool, Batch &batch, const unsigned *workers, unsigned count) {
			const uint32_t generation = ++pool.Generation;
			pool.Current = &batch;
			pool.Inside.store(count, std::memory_order_relaxed);
			std::fill(pool.Touched.begin(), pool.Touched.end(), uint8_t{0});
			for (unsigned at = 0; at < count; at++) {
				WorkerSlot &slot = pool.Slots[workers[at]];
				slot.Wake.store(generation, std::memory_order_seq_cst);
				if (slot.Asleep.load(std::memory_order_seq_cst)) {
					pool.Touched[workers[at] / BAND_WIDTH] = 1;
				}
			}
			Ring(pool.Bands.get(), pool.Touched);
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

			const bool pinned = platform::PinCurrentThread(pool.WorkerProcessors[workerIndex]);
			{
				std::lock_guard lock(pool.Guard);
				pool.WorkerPinned[workerIndex] = pinned ? 1 : 0;
				pool.Ready++;
				pool.ReadyCondition.notify_one();
			}

			WorkerSlot &slot = pool.Slots[workerIndex];
			SleepBand &band = pool.Bands[workerIndex / BAND_WIDTH];
			uint32_t seen = 0;
			for (;;) {
				seen = AwaitWake(slot, band, seen, workerIndex < pool.Spinners.load(std::memory_order_relaxed));
				if (pool.Stopping.load(std::memory_order_acquire)) {
					return;
				}

				// Only a worker the dispatcher counted into `Inside` is woken,
				// so the batch is live until this worker counts itself out.
				Batch &batch = *pool.Current;
				if (batch.AssignedWorkers == nullptr) {
					Drain(batch);
				} else {
					DrainAssigned(batch, workerIndex);
				}

				if (pool.Inside.fetch_sub(1, std::memory_order_acq_rel) == 1) {
					pool.Inside.notify_all();
				}
			}
		}

		void ShutdownPool(Pool &pool) {
			{
				std::lock_guard lock(pool.Guard);
				if (pool.Workers.empty()) {
					return;
				}
				pool.Stopping.store(true, std::memory_order_release);
			}
			for (size_t index = 0; index < pool.Workers.size(); index++) {
				pool.Slots[index].Wake.fetch_add(1, std::memory_order_seq_cst);
			}
			std::fill(pool.Touched.begin(), pool.Touched.end(), uint8_t{1});
			Ring(pool.Bands.get(), pool.Touched);

			for (std::thread &worker : pool.Workers) {
				worker.join();
			}

			std::lock_guard lock(pool.Guard);
			pool.Workers.clear();
			pool.WorkerProcessors.clear();
			pool.WorkerPinned.clear();
			pool.Ready = 0;
			pool.PinnedWorkers = 0;
			pool.Size.store(0, std::memory_order_relaxed);
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

		pool.Stopping.store(false, std::memory_order_relaxed);
		pool.Ready = 0;
		pool.PinnedWorkers = 0;
		pool.Slots = std::make_unique<WorkerSlot[]>(workers);
		const unsigned bands = (workers + BAND_WIDTH - 1) / BAND_WIDTH;
		pool.Bands = std::make_unique<SleepBand[]>(bands);
		pool.Touched.assign(bands, 0);
		pool.Identity.resize(workers);
		for (unsigned index = 0; index < workers; index++) {
			pool.Identity[index] = index;
		}
		pool.Named.reserve(workers);
		pool.NamedSeen.assign(workers, 0);
		pool.Workers.reserve(workers);
		pool.WorkerProcessors.resize(workers);
		pool.WorkerPinned.assign(workers, 0);
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
		pool.Size.store(workers, std::memory_order_relaxed);
		while (pool.PinnedWorkers < pool.WorkerPinned.size() && pool.WorkerPinned[pool.PinnedWorkers] != 0) {
			pool.PinnedWorkers++;
		}

		pool.Spinners.store(pool.PinnedWorkers, std::memory_order_relaxed);

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

		const size_t workers = pool.Size.load(std::memory_order_relaxed);
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

		// Do not rewrite the slot while a prior worker still holds it.
		WaitZero(pool.Inside);

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

		// Wake no more workers than the remaining ranges require. The lowest
		// indices are the ones pinned to distinct physical cores.
		const unsigned signaled = static_cast<unsigned>(std::min(ranges - 1, workers));
		Publish(pool, batch, pool.Identity.data(), signaled);
		if (core::FrameGraph::IsEnabled()) core::Metrics::Count("jobs.signaled_workers", signaled);

		{
			ENGINE_PROFILE_CAT("jobs.drain", core::ProfileCategory::Engine);

			Drain(batch, 0);
		}

		{
			// Waiting for the last range is idle time, not dispatch work.
			ENGINE_PROFILE_CAT("jobs.join", core::ProfileCategory::Idle);
			WaitZero(batch.Outstanding);
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
		// Written by Start before it returns, and Start must not race a dispatch.
		const unsigned pinnedWorkers = pool.PinnedWorkers;

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

		WaitZero(pool.Inside);
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

		// Wake only the workers the mapping names, in first-named order.
		pool.Named.clear();
		for (const unsigned worker : workerByIndex) {
			if (pool.NamedSeen[worker] == 0) {
				pool.NamedSeen[worker] = 1;
				pool.Named.push_back(worker);
			}
		}
		for (const unsigned worker : pool.Named) {
			pool.NamedSeen[worker] = 0;
		}
		Publish(pool, batch, pool.Named.data(), static_cast<unsigned>(pool.Named.size()));
		if (batch.CaptureAssigned) core::Metrics::Count("jobs.signaled_workers", pool.Named.size());

		{
			ENGINE_PROFILE_CAT("jobs.join.assigned", core::ProfileCategory::Idle);
			WaitZero(batch.Outstanding);

			// Wait for every woken worker to leave, so none still reads the
			// mapping once its caller returns.
			WaitZero(pool.Inside);
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
