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

			// Ranges not yet finished, **counted in ranges rather than in
			// workers**, and that distinction was worth 0.35 ms a frame.
			//
			// It used to be the worker count, so the join could not return until
			// every thread in the pool had woken, taken `Pool::Guard` to read the
			// batch, found nothing left to claim, and taken the guard a second
			// time to decrement. Two worlds dispatched across twenty-three
			// workers meant forty-six serialised acquisitions of one contended
			// mutex on the barrier's critical path to run two ticks — a cost
			// linear in `hardware_concurrency` and independent of the work, which
			// is why it was invisible on a four-thread box and a third of the
			// frame on a real one.
			//
			// Counting ranges makes the join ask the only question it has: is the
			// work done. Which threads showed up, and how many of them found
			// nothing, stops being the barrier's business — see `Pool::Inside`
			// for the part that still is.
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

		// Whether every dispatch is being forced onto its caller's thread.
		//
		// **Process-wide and not per thread**, because the thing it is for is a
		// frame graph, and a frame graph is a picture of one process. A worker
		// that had its own answer could still open a batch of its own and drop
		// the spans inside it, which is the exact hole this closes.
		//
		// Relaxed on both sides. Turning this on does not need to be ordered
		// against anything — a dispatch that reads the old value runs the way it
		// was always going to, and the next one reads the new value. The cost of
		// making it stronger would be paid by every dispatch forever to make a
		// switch somebody flips by hand land one batch sooner.
		std::atomic<bool> Forced{false};

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

			// Signalled when the last worker leaves `Drain`. See `Inside`.
			std::condition_variable Drained;

			// The one batch, owned by the pool rather than by the `For` that
			// dispatched it.
			//
			// **It was a local, and a range-counted join makes a local unsound.**
			// Once the barrier stops waiting for every worker, `For` can return
			// while a worker is still on its way into `Drain` — and that worker
			// holds a pointer to the batch. A stack frame that has returned is
			// not memory it may read, and `Batch::Body` pointed at the caller's
			// `std::function`, which is not a callable it may reach either.
			//
			// One slot is enough because `Claimed` already admits one batch at a
			// time; a second dispatch runs inline and shares nothing.
			Batch Slot;

			// Workers between reading `Current` and leaving `Drain`.
			//
			// **This is the wait that used to be the join, moved to where it is
			// free.** Somebody still has to know when the last straggler has let
			// go of the batch — but the thread that needs to know is not the one
			// finishing this frame's tick, it is the one starting the *next*
			// dispatch, and that is a frame away. By then every worker woken for
			// the last batch has long since found nothing, decremented, and gone
			// back to sleep, so the wait is satisfied the moment it is asked.
			//
			// The stragglers still pay their two lock acquisitions each. They now
			// pay them alongside the interface build instead of in front of it.
			size_t Inside = 0;

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

		// No range. Distinct from index zero, which is a real one.
		constexpr size_t NO_RANGE = static_cast<size_t>(-1);

		// Runs one already-claimed range, without retiring it.
		void RunBody(Batch &batch, size_t begin, uint64_t &busy) {
			const size_t end = std::min(begin + batch.Grain, batch.Count);

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

		// Marks one range finished, releasing the join when it was the last.
		//
		// Called once the body has returned *or* thrown: a range that failed is
		// finished, and a batch whose join waited on an abandoned range would
		// hang rather than rethrow.
		void Retire(Batch &batch) {
			// **The guard is taken by the last range only, never by the others.**
			// That is the difference from the old join: retiring used to cost an
			// acquisition of `Pool::Guard` per *worker*, including the ones that
			// had no range, and every one of them was in front of the dispatcher.
			// Now the mutex is touched once per batch, by whichever thread
			// happened to finish last, and it is taken rather than skipped
			// because a notify racing the waiter's predicate check is a lost
			// wakeup and therefore a hang.
			if (batch.Outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				Pool &pool = Get();
				std::lock_guard lock(pool.Guard);
				pool.Finished.notify_all();
			}
		}

		// Claims ranges until there are none left. Run by the workers and by
		// the calling thread, which is why the caller never idles.
		//
		// `first` is a range the caller claimed before calling, or `NO_RANGE`.
		//
		// **One range is always held back unretired, and that is what keeps the
		// reported timings whole.** The retire that empties `Outstanding` is
		// what lets `For` return and read `BusyNanoseconds` — so a thread that
		// retired its last range and *then* added its total would be racing the
		// dispatcher for it, and `worlds (workers)` would silently lose whichever
		// worker finished closest to the barrier. Keeping one in hand means the
		// decrement that can release the join is always the one after this
		// thread's numbers have landed.
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

				// Safe to let the previous one go now: this thread holds another,
				// so `Outstanding` cannot reach zero on this line.
				if (pending != NO_RANGE) {
					Retire(batch);
				}

				RunBody(batch, begin, busy);
				took = true;
				pending = begin;
				begin = NO_RANGE;
			}

			// Once per participant rather than once per range: a relaxed add
			// per range would be a contended cache line in the innermost loop
			// of the job system, which is the one place that cannot afford one.
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

					// **Under the same lock that read `Current`, which is the
					// whole of the lifetime argument.** The dispatcher clears
					// `Current` and rewrites the slot under this guard too, so a
					// worker either gets here first — and is counted, so the next
					// dispatch waits for it — or arrives to a null pointer and
					// has nothing to hold.
					if (batch != nullptr) {
						pool.Inside++;
					}
				}

				if (batch != nullptr) {
					// A worker that wakes after the batch is finished finds
					// `Next` past `Count` and returns without touching `Body`,
					// which is what makes a straggler harmless rather than a
					// call into a dead `std::function`.
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

		// **Before the pool is even reached, and before its lock is taken.**
		// The whole point of the flag is that the caller's thread does the work,
		// so consulting the pool first would be asking a question whose answer
		// cannot change what happens. It also makes the forced path cheaper than
		// the ordinary inline one, which is a happy accident rather than a
		// reason: this is a measurement instrument, not a fast path.
		if (Forced.load(std::memory_order_relaxed)) {
			const uint64_t started = core::Clock::Nanoseconds();
			body(0, count);
			LastTiming = Inline(core::Clock::Nanoseconds() - started);
			return;
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

		const size_t ranges = (count + grain - 1) / grain;

		Batch &batch = pool.Slot;

		{
			std::unique_lock lock(pool.Guard);

			// **The straggler wait, and it is here rather than in the join.**
			// See `Pool::Inside`: the previous batch's leftovers have had a whole
			// frame to notice they had nothing to do, so this is a predicate that
			// is already true, not a barrier. It has to be checked somewhere,
			// though — and it has to be checked *before* the slot is rewritten,
			// because a worker still reading `Next` and `Count` from the last
			// batch would otherwise see them replaced underneath it and claim a
			// range of the new one that nothing counted.
			pool.Drained.wait(lock, [&] { return pool.Inside == 0; });

			// Written under the guard for the same reason: a worker reads
			// `Current` while holding it, so nobody can be looking at the slot
			// between these two lines.
			batch.Body = &body;
			batch.Count = count;
			batch.Grain = grain;
			batch.Outstanding.store(ranges, std::memory_order_relaxed);
			batch.BusyNanoseconds.store(0, std::memory_order_relaxed);
			batch.Participants.store(0, std::memory_order_relaxed);
			batch.Failure = nullptr;

			// **`Next` starts one range in, not at zero: the dispatcher's own
			// share, reserved before the pool is woken.** The comment below has
			// always said this thread is a participant rather than a supervisor,
			// and measured it was not — `notify_all` ran first, so on a wide pool
			// the workers had claimed every range before this thread's own
			// `fetch_add` landed. `jobs.drain` read 0.00 while `jobs.join` read
			// the entire barrier: the dispatcher paid the wait and did none of
			// the work. Reserving here makes the claim true by construction.
			batch.Next.store(grain, std::memory_order_relaxed);

			pool.Current = &batch;
			pool.Generation++;
		}

		// **As many workers as there are ranges left, not as many as exist.**
		// Waking twenty-three threads to run two world ticks is twenty-one
		// threads whose entire contribution is a `fetch_add` that finds nothing
		// and two acquisitions of a contended mutex. The range count is what the
		// pool can actually absorb, and one of the ranges is already this
		// thread's.
		//
		// Under-waking cannot lose work — the dispatcher drains until the batch
		// is empty, so an unwoken worker costs parallelism and never a range.
		// That is what makes this safe to tune where the old worker-counted join
		// would have hung.
		if (ranges > workers) {
			pool.Available.notify_all();
		} else {
			for (size_t woken = 1; woken < ranges; woken++) {
				pool.Available.notify_one();
			}
		}

		{
			// The dispatching thread's own share. It is a participant, not a
			// supervisor, so this is real work and belongs beside the ranges
			// the workers took rather than folded into the wait below.
			ENGINE_PROFILE_CAT("jobs.drain", core::ProfileCategory::Engine);

			Drain(batch, 0);
		}

		{
			// **`Idle`, and this is the one that was worth finding.** A
			// fork-join barrier is the dispatching thread doing nothing until
			// the slowest range lands, and it read as busy engine time — so an
			// imbalanced batch, which is the failure this pool actually has,
			// looked identical to a batch that was simply large. The overlay
			// subtracts idle to get its busy figure, so this now leaves the
			// numerator the moment it starts blocking.
			//
			// It waits on ranges now, so what is left here is genuine imbalance:
			// the slowest world still running while this thread has nothing to
			// take. The pool's own wake-up no longer appears in it.
			ENGINE_PROFILE_CAT("jobs.join", core::ProfileCategory::Idle);
			std::unique_lock lock(pool.Guard);
			pool.Finished.wait(lock, [&] { return batch.Outstanding.load(std::memory_order_acquire) == 0; });

			// Cleared so a worker still on its way in reads null and never
			// counts itself, which keeps `Inside` down to the threads that
			// genuinely overlapped the batch.
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
		// **The pool is left running.** Stopping it would make this a decision
		// taken once at startup rather than a switch, and the case that matters
		// is flipping it between two frames of one session and reading both
		// flame graphs. Idle workers cost a condition variable each.
		Forced.store(forced, std::memory_order_relaxed);
	}

	bool ForceSerialCompute() {
		return Forced.load(std::memory_order_relaxed);
	}
}
