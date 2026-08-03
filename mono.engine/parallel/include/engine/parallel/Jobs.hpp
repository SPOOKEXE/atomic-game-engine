#pragma once

// Engine-internal blocking dispatch over one process-wide worker pool.
//
// This is not the userland `thread` datatype or process dispatch. Jobs are
// fork-joined inside the call that starts them and cannot outlive that call.
//
// One batch occupies the pool at a time. A second dispatch — nested, or from
// another thread — is not refused and does not wait: it runs its span inline on
// the thread that asked. So the pool never has to be reasoned about globally,
// only locally, and the worst case is serial rather than wrong.
//
// @tier L2 · shared

#include <cstddef>
#include <cstdint>
#include <functional>

namespace engine::parallel {

	// What one dispatch of `Jobs::For` cost.
	//
	// **Busy time is not wall time and the difference is the point.** Eight
	// workers doing a millisecond each in one millisecond of wall clock is
	// eight milliseconds of `Busy` and one of `Wall`; the ratio is how much of
	// the machine the batch actually used. A profiler that only had the wall
	// figure could not tell that batch from one worker doing nothing seven
	// times.
	//
	// @since v0.2
	struct BatchTiming {
		// Work done, summed across every thread that took a range.
		float BusyMilliseconds = 0.0f;

		// Wall time the dispatch took, measured by the thread that dispatched.
		float WallMilliseconds = 0.0f;

		// How many threads took at least one range. One means it ran inline —
		// too little work to hand over, no workers, or another dispatch already
		// held the pool.
		uint32_t Participants = 0;
	};

	// Dispatches one blocking parallel-for batch across the caller and a worker
	// pool.
	//
	// The pool is process-wide and admits only one batch at a time. There are no
	// handles: every range finishes before For returns.
	class Jobs {
	  public:
		// Starts the process-wide worker pool if it is not already running.
		//
		// The worker count excludes the calling thread, which also drains every
		// For batch. `Start(0)` creates one fewer worker than reported hardware
		// threads, or no workers when fewer than two hardware threads are reported.
		// Calling Start again while the pool is running leaves it unchanged.
		//
		// @param workers Worker threads to create, or zero to choose automatically.
		// @threadsafe
		static void Start(unsigned workers = 0);

		// Stops and joins every worker, leaving the pool empty.
		//
		// This does nothing when no pool is running and is safe to repeat. Call it
		// only after the active For has returned; Stop does not cancel a batch. Do
		// not call Stop concurrently with another Jobs operation.
		static void Stop();

		// Reports how many pool workers exist, excluding the calling thread.
		//
		// @return Zero before Start and after Stop; otherwise the count selected by
		//         Start.
		// @threadsafe
		static unsigned WorkerCount();

		// Runs `body` over `[0, count)` and blocks until every range has finished.
		//
		// The caller drains ranges alongside the pool. With no workers, or when
		// `count` is no greater than the effective grain, the full span runs once
		// on the caller. Otherwise the pool claims contiguous, non-overlapping
		// ranges of at most `grain` indices. A zero count does not invoke `body`;
		// a zero grain selects DEFAULT_GRAIN.
		//
		// **Only one batch occupies the pool at a time, and a call that finds it
		// occupied runs its whole span inline on the calling thread.** That
		// covers a nested For from inside `body` and two threads dispatching at
		// once, and neither deadlocks. Which of two racing callers wins is not
		// defined; both return with every index visited.
		//
		// Inline and pooled execution are **observationally identical**. A body
		// may only write what its own range names, so the same span run whole on
		// one thread produces the same bytes as the same span split across
		// twenty. Losing the pool costs wall time and changes no result, which
		// is what makes a world tick safe to run as a range inside a larger
		// batch: the world's own parallel loops degrade to serial instead of
		// corrupting the batch that dispatched them.
		//
		// If a pooled invocation throws, the failed range is abandoned, the
		// other ranges finish, and the first captured exception is rethrown on
		// the caller. An inline invocation rethrows directly. Either way the
		// pool is released, so a throwing batch does not strand it.
		//
		// @param count Number of indices in the half-open span `[0, count)`.
		// @param grain Maximum indices per pooled range and the inline cutoff, or
		//              zero for DEFAULT_GRAIN.
		// @param body  Callable given each half-open range `[begin, end)`; shared
		//              captures must permit concurrent access.
		// @tick
		// @threadsafe
		// @param minimum Indices below which the span runs inline whatever the
		//                grain says. Zero derives it from the grain, which
		//                assumes one index is cheap. A caller whose unit of
		//                work is already expensive — a world tick, a chunk
		//                compression — passes its own and gets dispatched at
		//                the count where that actually pays.
		static void
		For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body, size_t minimum = 0);

		// What the calling thread's most recent `For` cost.
		//
		// **This is how parallel work reaches the frame graph.** A worker
		// cannot record its own span — `FrameGraph::Push` refuses anything off
		// the frame's owning thread, and locking there would put contention on
		// every span of every frame — so the workers measure themselves, the
		// dispatch sums what they reported, and the caller hands the number to
		// `FrameGraph::Report`. The graph plots the latest timing received
		// rather than a clock reading that belongs to another thread.
		//
		// Per calling thread, so two threads dispatching at once each read
		// their own. Reset by every `For`, including the ones that ran inline.
		//
		// @return The last dispatch's timing, or zeroes before the first.
		// @threadsafe
		static BatchTiming LastBatch();

		// Default pooled range size for cheap per-index work, and — through
		// MINIMUM_GRAINS — the count below which nothing is dispatched at all.
		//
		// **Kept at 4096, having been re-measured at `-O3` and found wrong in
		// both directions at once.** For the cheapest body there is, three float
		// adds per row, `engine.ecs.bench.iteration` puts the crossover near
		// 262,144 rows — this default's floor is 32,768, and at that count the
		// dispatched loop measures 31 us against 5.5 us run serially. Raising the
		// grain to 32,768 would move the floor to where that body wants it and
		// break the only long-lived caller that takes the default:
		// `mono.client/src/Replicated.cpp` writes a `CFrame`, two vectors and two
		// ids per row where this body writes three floats, so its span is worth
		// dispatching an order of magnitude sooner and a raised floor would
		// refuse a handover that pays.
		//
		// **One number cannot move in two directions, and this one is two
		// numbers wearing one name** — the range size once dispatched, and the
		// count at which to dispatch. So it stays where it is, and a caller whose
		// row cost is not this row's cost passes its own grain.
		// `physics::INTEGRATE_GRAIN` is what that looks like, measured.
		static constexpr size_t DEFAULT_GRAIN = 4096;

		// How many grains of work a span must hold before the pool is woken.
		//
		// **Waking the pool costs the same whatever the work is, and it is
		// bigger than it reads.** `engine.parallel.bench.dispatch` measures an
		// empty dispatch at 31 us against 48 ns for the decision not to
		// dispatch, and at 2.3 us against a pool of one — so the cost is about
		// 1.3 us per *worker*, and only about 95 ns per range. It is linear in
		// the pool because every worker decrements `Batch::Outstanding` under
		// `Pool::Guard` whether it took a range or not; that join, not the
		// notify, is what a short span cannot repay.
		//
		// **The crossover is an amount of time, and this expresses it as a row
		// count.** Re-measured at `-O3`: three float adds per row cross near
		// 262,144 rows, and `physics::IntegrateMotion`'s `CFrame` step near
		// 8,000. Thirty-two times apart in rows; 49 us and 29 us in serial work,
		// which is one handover either way. A row count can only be right for
		// one row cost, and nothing in the signature can know that cost — which
		// is why both callers that measured pass a grain of their own, and why
		// `minimum` exists for the ones whose index is not a row at all.
		//
		// **Eight is kept rather than raised, because it multiplies every
		// caller's floor and one of those was measured.** `physics` passes 1024
		// and wants its floor at 8192; a MINIMUM_GRAINS of 64 would put it at
		// 65,536 and hand back the 1.8x that module measures at 12,000 rows.
		// Eight also has a reason of its own: dispatching two or three ranges
		// leaves most of the pool idle and pays the whole wake cost anyway.
		//
		// **It is a default and not a law.** It infers the cost of one index
		// from the grain, which is right for rows and wrong for anything whose
		// unit of work is already large — a world tick is one index and tens of
		// microseconds. Those callers pass `minimum` and say so; measured, four
		// world ticks are 1.9x faster dispatched than run inline, and this rule
		// alone would have refused to dispatch them.
		static constexpr size_t MINIMUM_GRAINS = 8;
	};
}
