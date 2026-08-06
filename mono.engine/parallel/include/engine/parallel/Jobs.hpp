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
		// dispatch, and at 2.3 us against a pool of one — so the cost was about
		// 1.3 us per *worker*, and only about 95 ns per range. It was linear in
		// the pool because every worker decremented `Batch::Outstanding` under
		// `Pool::Guard` whether it took a range or not; that join, not the
		// notify, was what a short span could not repay.
		//
		// **The per-worker term is gone and this number has not been re-derived
		// from it.** The barrier counts ranges rather than workers now, and only
		// as many workers are woken as there are ranges to give them, so a
		// dispatch no longer pays for the threads it had no work for — a two-
		// range batch across twenty-three workers measured 0.35 ms of pure join
		// in `studio`'s frame graph and should now measure the imbalance alone.
		// The floor below is still the measured one, so it is now conservative
		// rather than wrong: it refuses handovers that may well pay. Re-measure
		// `engine.parallel.bench.dispatch` before moving it.
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

	// --- forcing the whole compute pipeline onto one thread ------------------

	// Makes every parallel dispatch in the process run on the thread that asked.
	//
	// **This exists because the profiler cannot see a worker thread, and that is
	// not a bug in the profiler.** `core::FrameGraph::Push` refuses a span opened
	// off the frame's owning thread — locking there would put contention on every
	// span of every frame — so a world ticking on a worker contributes one
	// reported aggregate and drops every span inside it. `Universe::Tick` says so
	// where it reports "worlds (workers)", and `studio`'s frame graph says so
	// again in its dropped-span line.
	//
	// The consequence is that the most expensive thing the engine does is the
	// one thing whose *shape* cannot be read. Turning this on puts every span
	// back on the owning thread, and the flame graph becomes the whole tick
	// rather than one bar labelled with a number.
	//
	// **A runtime switch and emphatically not a build option.** The only build
	// worth profiling is an optimised one, and a `#ifdef` would compile this out
	// of exactly that build. It is also what lets one binary produce both
	// readings, so the two numbers differ by the flag and not by the compiler.
	//
	// **Wall time is expected to get worse and that is the trade.** This is a
	// measurement instrument: it makes the frame slower and legible. A number
	// read with it on is a *serial* cost — useful for finding which stage is
	// expensive, useless for judging whether the parallel version is fast.
	//
	// Two things go serial and they are separate mechanisms:
	//
	//   - `Jobs::For` runs its whole span inline, whatever the pool holds. The
	//     pool is left running rather than stopped, so the flag can be turned on
	//     and off between frames.
	//   - `world::Universe::Tick` takes its serial branch, so it opens a
	//     "worlds (serial)" scope instead of reporting an aggregate the flame
	//     graph would then double-count against the spans underneath it.
	//
	// Everything else is unaffected, and the name says `compute` rather than
	// `threads` for that reason: the render thread, SDL's threads and the
	// profiler's own are still there. What goes serial is the work whose shape
	// somebody is trying to read.
	//
	// @param forced Whether to force every dispatch inline.
	// @threadsafe
	// @since v0.8
	void SetForceSerialCompute(bool forced);

	// Whether every parallel dispatch is being forced inline.
	//
	// @return `true` when `SetForceSerialCompute(true)` is in effect.
	// @threadsafe
	bool ForceSerialCompute();
}
