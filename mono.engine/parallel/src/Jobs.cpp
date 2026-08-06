#include <engine/core/Clock.hpp>
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
			std::mutex Guard;
			std::condition_variable Available;
			std::condition_variable Finished;

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
			bool Stopping = false;
		};

		Pool &Get() {
			static Pool pool;
			return pool;
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

		void WorkerLoop() {
			auto &pool = Get();
			uint64_t seen = 0;

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
					Drain(*batch);

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
		std::lock_guard lock(pool.Guard);

		if (!pool.Workers.empty()) {
			return;
		}

		if (workers == 0) {
			const unsigned available = std::thread::hardware_concurrency();
			// Leave one core for the participating caller.
			workers = available > 1 ? available - 1 : 0;
		}

		pool.Stopping = false;
		pool.Workers.reserve(workers);
		for (unsigned index = 0; index < workers; index++) {
			pool.Workers.emplace_back(WorkerLoop);
		}

		ENGINE_INFO("job system started with {} worker(s)", workers);
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

		for (auto &worker : pool.Workers) {
			worker.join();
		}
		pool.Workers.clear();
	}

	unsigned Jobs::WorkerCount() {
		auto &pool = Get();
		std::lock_guard lock(pool.Guard);
		return static_cast<unsigned>(pool.Workers.size());
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
