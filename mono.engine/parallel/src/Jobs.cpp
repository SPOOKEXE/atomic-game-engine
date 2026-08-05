#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace engine::parallel {

	namespace {

		// One batch in flight at a time. The pool exists to make a single
		// parallel-for wide, not to overlap several of them.
		//
		// The shape is refused rather than defended against, but it is refused
		// *safely*: a second dispatch runs on the thread that asked for it
		// instead of corrupting the first. See Pool::Claimed.
		struct Batch {
			const std::function<void(size_t, size_t)> *Body = nullptr;
			size_t Count = 0;
			size_t Grain = 0;
			std::atomic<size_t> Next{0};
			std::atomic<size_t> Outstanding{0};

			std::mutex FailureGuard;
			std::exception_ptr Failure;

			// Summed across every thread that took a range, so the dispatcher
			// can hand one number to the frame graph rather than the graph
			// trying to time threads it does not own.
			std::atomic<uint64_t> BusyNanoseconds{0};
			std::atomic<uint32_t> Participants{0};
		};

		// The calling thread's most recent dispatch. Per thread, because two
		// threads may dispatch at once and each wants its own answer.
		thread_local BatchTiming LastTiming;

		// A dispatch that ran on the calling thread: one participant, and busy
		// equal to wall because there was nowhere else for the time to go.
		BatchTiming Inline(uint64_t nanoseconds) {
			const auto milliseconds = static_cast<float>(static_cast<double>(nanoseconds) / 1e6);
			return BatchTiming{milliseconds, milliseconds, 1};
		}

		struct Pool {
			std::vector<std::thread> Workers;
			std::mutex Guard;
			std::condition_variable Available;
			std::condition_variable Finished;

			// Whether a batch owns the pool right now.
			//
			// Not redundant with `Current`, and the difference is the whole
			// point: `Current` is written *after* the winner has decided to
			// dispatch, so two callers reaching that write both believed they
			// had the pool. The second overwrote the pointer the workers were
			// draining through and the first waited on an `Outstanding` count
			// being decremented for somebody else's batch — a hang, or the
			// wrong body run against the wrong range.
			//
			// Claiming first turns that into a decision: the loser runs its
			// span itself and nothing is shared.
			std::atomic<bool> Claimed{false};

			Batch *Current = nullptr;
			uint64_t Generation = 0;
			bool Stopping = false;
		};

		Pool &Get() {
			static Pool pool;
			return pool;
		}

		// Claims ranges until there are none left. Run by the workers and by
		// the calling thread, which is why the caller never idles.
		void Drain(Batch &batch) {
			uint64_t busy = 0;
			bool took = false;

			for (;;) {
				const size_t begin = batch.Next.fetch_add(batch.Grain, std::memory_order_relaxed);
				if (begin >= batch.Count) {
					break;
				}

				const size_t end = std::min(begin + batch.Grain, batch.Count);
				took = true;

				// Around the body only. The claim, the wait and the bookkeeping
				// are the dispatch's cost and belong to the thread that paid
				// them; what a worker reports is the work.
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

			// Once per participant rather than once per range: a relaxed add
			// per range would be a contended cache line in the innermost loop
			// of the job system, which is the one place that cannot afford one.
			if (took) {
				batch.BusyNanoseconds.fetch_add(busy, std::memory_order_relaxed);
				batch.Participants.fetch_add(1, std::memory_order_relaxed);
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
				}

				if (batch) {
					Drain(*batch);

					std::lock_guard lock(pool.Guard);
					if (batch->Outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						pool.Finished.notify_all();
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
			// The calling thread participates, so one fewer worker keeps the
			// total at the core count rather than one over it.
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

		auto &pool = Get();

		// Inline when there is nobody to hand work to, or too little work to be
		// worth the handover. Both cases are the common one in a small scene,
		// and taking the lock at all would be the dominant cost.
		size_t workers = 0;
		{
			std::lock_guard lock(pool.Guard);
			workers = pool.Workers.size();
		}
		// `MINIMUM_GRAINS` rather than one grain. A span just over the grain
		// dispatched two chunks and paid the full cost of waking the pool to do
		// it — which measured five times slower than simply running it here.
		//
		// Derived from the grain unless the caller said otherwise, because only
		// the caller knows whether one index is a row or a whole world.
		const size_t floor = minimum > 0 ? minimum : grain * Jobs::MINIMUM_GRAINS;
		if (workers == 0 || count < floor) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		// Claim the pool, or run the span here.
		//
		// A losing caller is not an error and not a fallback for a broken case:
		// it is how a nested For and two concurrently dispatching threads are
		// both made safe without a work-stealing deque. The cost is parallelism
		// for that one dispatch, never correctness — every body is already
		// required to write only what its own range names, so a span run whole
		// on one thread produces the same bytes as the same span split across
		// twenty.
		//
		// A world tick is a range in somebody else's batch once `world` exists,
		// which is what makes this the ordinary case rather than the odd one.
		if (pool.Claimed.exchange(true, std::memory_order_acquire)) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
		}

		// Releases on every path out, including the one where a range threw and
		// the exception is on its way back to the caller.
		struct ClaimGuard {
			Pool &Owner;
			~ClaimGuard() {
				Owner.Claimed.store(false, std::memory_order_release);
			}
		} claim{pool};

		const uint64_t dispatched = core::Clock::Nanoseconds();

		Batch batch;
		batch.Body = &body;
		batch.Count = count;
		batch.Grain = grain;
		batch.Outstanding.store(workers, std::memory_order_relaxed);

		{
			std::lock_guard lock(pool.Guard);
			pool.Current = &batch;
			pool.Generation++;
		}
		pool.Available.notify_all();

		{
			// The dispatching thread's own share. It is a participant, not a
			// supervisor, so this is real work and belongs beside the ranges
			// the workers took rather than folded into the wait below.
			ENGINE_PROFILE_CAT("jobs.drain", core::ProfileCategory::Engine);
			Drain(batch);
		}

		{
			// **`Idle`, and this is the one that was worth finding.** A
			// fork-join barrier is the dispatching thread doing nothing until
			// the slowest range lands, and it read as busy engine time — so an
			// imbalanced batch, which is the failure this pool actually has,
			// looked identical to a batch that was simply large. The overlay
			// subtracts idle to get its busy figure, so this now leaves the
			// numerator the moment it starts blocking.
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
}
