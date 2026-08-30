#pragma once

// Engine-internal blocking dispatch over one process-wide worker pool.
//
// This is not the userland `thread` datatype or process dispatch. Jobs are
// fork-joined inside the call that starts them and cannot outlive that call.
//
// One batch occupies the pool at a time. A second dispatch - nested, or from
// another thread - is not refused and does not wait: it runs its span inline on
// the thread that asked. So the pool never has to be reasoned about globally,
// only locally, and the worst case is serial rather than wrong.
//
// @tier L2 · shared

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace engine::parallel {

	// Where a job's work executes.
	//
	// Arbitrary callbacks support `Serial` and `Threaded`. `Processed` is for
	// typed jobs whose inputs and outputs have an explicit byte format, because
	// a closure and the memory it captures have no meaning in another process.
	//
	// @since v0.20
	enum class JobContext : uint8_t {
		Serial,
		Threaded,
		Processed,
	};

	// Returns the stable spelling used by scripts, settings and reports.
	const char *Describe(JobContext context);

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

		// How many threads took at least one range. One means it ran inline -
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
		// For batch. `Start(0)` creates one fewer worker than the logical processors
		// available to this process, or no workers when fewer than two are available.
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

		// Reports how many leading workers are pinned to distinct physical cores.
		//
		// Assigned-worker dispatch uses only this prefix. A zero means the pool is
		// absent or the platform could not establish a core binding, so a
		// caller requiring placement must use its serial fallback.
		//
		// @return The number of workers with verified, distinct bindings.
		// @threadsafe
		static unsigned PinnedWorkerCount();

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
		//                work is already expensive - a world tick, a chunk
		//                compression - passes its own and gets dispatched at
		//                the count where that actually pays.
		static void
		For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body, size_t minimum = 0);

		// Runs a batch in an explicit execution context.
		//
		// `Serial` always runs inline. `Threaded` has the same blocking fork-join
		// contract as the overload above. `Processed` throws `invalid_argument`:
		// process work must use a typed protocol rather than capture live memory.
		//
		// @param context Where the work may execute.
		// @param count Number of indices in `[0, count)`.
		// @param grain Maximum indices per threaded range.
		// @param body Callable given each half-open range.
		// @param minimum Threaded dispatch floor, or zero to derive it.
		// @tick
		// @threadsafe
		static void
		For(JobContext context,
			size_t count,
			size_t grain,
			const std::function<void(size_t, size_t)> &body,
			size_t minimum = 0);

		// Runs each index on the pinned worker named for it and blocks until done.
		//
		// Unlike For(), the calling thread never takes a range. Each worker in the
		// pinned prefix has one distinct physical-core binding, so tasks assigned to
		// different workers cannot execute on sibling logical CPUs. Several tasks
		// may name one worker; that worker executes them serially in index order.
		// This is the bounded answer when there are more tasks than physical cores.
		//
		// The mapping is consumed only during the call. Every index must name a
		// worker below PinnedWorkerCount(). With no pinned workers, a bad mapping,
		// forced serial compute, or a competing batch, the whole span runs inline
		// on the caller. Inline execution changes placement and no result.
		//
		// A pooled exception is captured and rethrown on the caller after every
		// other task has finished, matching For(). A nested For() from an assigned
		// task runs inline because this batch owns the process-wide pool.
		//
		// @param workerByIndex The pinned worker assigned to every task index.
		// @param body          Callable receiving either one assigned index or the
		//                      full span for an inline fallback.
		// @tick
		// @threadsafe
		static void
		ForWorkers(std::span<const unsigned> workerByIndex, const std::function<void(size_t, size_t)> &body);

		// What the calling thread's most recent `For` cost.
		//
		// **This is how parallel work reaches the frame graph.** A worker
		// cannot record its own span - `FrameGraph::Push` refuses anything off
		// the frame's owning thread, and locking there would put contention on
		// every span of every frame - so the workers measure themselves, the
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

		// Default pooled range size for cheap per-index work, and - through
		// MINIMUM_GRAINS - the count below which nothing is dispatched at all.
		//
		// **Kept at 4096, having been re-measured at `-O3` and found wrong in
		// both directions at once.** For the cheapest body there is, three float
		// adds per row, `engine.ecs.bench.iteration` puts the crossover near
		// 262,144 rows - this default's floor is 32,768, and at that count the
		// dispatched loop measured 31 us against 5.5 us run serially. That 31 us
		// predates the pool's join rewrite and MINIMUM_GRAINS below carries the
		// current figure; it is a smaller loss now and still a loss. Raising the
		// grain to 32,768 would move the floor to where that body wants it and
		// break the long-lived caller the default was defended for:
		// `mono.client/src/Scene.cpp`'s draw-list loop writes a `CFrame`, two
		// vectors and two ids per row where this body writes three floats, so its
		// span is worth dispatching an order of magnitude sooner and a raised
		// floor would refuse a handover that pays.
		//
		// **That citation named `mono.client/src/Replicated.cpp` until v0.8 and
		// the file was wrong, not the argument.** `Replicated.cpp` contains no
		// parallel dispatch at all - it walks with `Store::Each` and says at its
		// own comment why a batched walk cannot serve it - and the body being
		// described is `scene::DrawInstance`, which is written by the loop above.
		// The loop changed files and the reference did not follow.
		//
		// **And that loop now passes 1024 of its own, which sharpens this rather
		// than retiring it.** The caller that could name its cost stopped taking
		// the default, so what is left taking it is the callers that have not
		// measured - `scene::CapturePreviousTransforms` is one, and says so at
		// its own call. Raising the grain to suit the cheap body would refuse
		// every one of their handovers on the strength of a measurement taken
		// against a body none of them runs.
		//
		// **One number cannot move in two directions, and this one is two
		// numbers wearing one name** - the range size once dispatched, and the
		// count at which to dispatch. So it stays where it is, and a caller whose
		// row cost is not this row's cost passes its own grain.
		// `physics::INTEGRATE_GRAIN` is what that looks like, measured.
		static constexpr size_t DEFAULT_GRAIN = 4096;

		// How many grains of work a span must hold before the pool is woken.
		//
		// **Waking the pool costs the same whatever the work is, and it is
		// bigger than it reads.** `engine.parallel.bench.dispatch` measured an
		// empty dispatch at 31 us against 48 ns for the decision not to
		// dispatch, and at 2.3 us against a pool of one - so the cost was about
		// 1.3 us per *worker*, and only about 95 ns per range. It was linear in
		// the pool because every worker decremented `Batch::Outstanding` under
		// `Pool::Guard` whether it took a range or not; that join, not the
		// notify, was what a short span could not repay.
		//
		// **The idle-worker term is gone, and it has been re-measured.** The
		// barrier counts ranges rather than workers now, and only as many
		// workers are woken as there are ranges to give them, so a dispatch no
		// longer pays for the threads it had no work for - a two-range batch
		// across twenty-three workers measured 0.35 ms of pure join in
		// `studio`'s frame graph. `engine.parallel.bench.dispatch` at `-O3`, on
		// twenty-four logical processors, against the figures above:
		//
		// | ranges | workers woken | |
		// |---|---|---|
		// | below the floor, so inline | 0 | 50 ns |
		// | 8 | 1 | 793 ns |
		// | 8 | 7 | 8.71 us |
		// | 128 | 4 | 6.11 us |
		// | 128 | 23 | 27.37 us |
		// | 1024 | 23 | 66.51 us |
		//
		// **It is linear in the workers a dispatch wakes, and only weakly in
		// the ranges** - which the first four rows of the table above could not
		// have told apart, because `For` wakes `ranges - 1` workers up to the
		// pool size and every one of them varied both at once. The last two
		// pairs separate them: holding the ranges at 128 and dropping the pool
		// from 23 workers to 4 takes 27.37 us to 6.11 us, while holding the
		// pool at 23 and taking the ranges from 128 to 1024 costs only 43.7 ns
		// each. So a dispatch is about **1.1 us a woken worker and 44 ns a
		// range**, and the per-worker term is twenty-five times the other one.
		//
		// **That term is additive across workers on distinct cores, which is
		// what makes it a serialisation rather than a wake latency.** Seven
		// woken workers cost seven times one, and twenty-three cost twenty-three
		// times it; a cost paid concurrently would not add up like that. What
		// every woken worker does in turn is take `Pool::Guard` twice - once to
		// read the batch and count itself in, once to count itself out - and the
		// join's own notify queues on the same mutex behind them. That is what
		// to attack if the floor ever has to come down, and it is a rewrite of
		// the join rather than a change to a constant.
		//
		// **The constant stays at 8, because the floor's job is the cheap body
		// and it is still doing it.** A cheaper handover is an argument for
		// lowering this only if something was being refused that now pays, and
		// the body this floor is calibrated against is not it: three float adds
		// per row cross near 262,144 rows and the floor sits at 32,768, where
		// `engine.ecs.bench.iteration` measured 5.5 us of serial work. Against
		// 31 us of handover that was a measured 5.7x loss; against 7.74 us it
		// works out at 1.4x, which is arithmetic rather than a reading because
		// the ECS suite has not been re-run. Smaller either way, and still a
		// loss on both - and lowering the floor would move
		// that body's dispatch earlier still, which is the wrong direction from
		// a number that is already eight times too eager for it.
		//
		// **So the answer for the callers this got cheaper for is a grain, not a
		// smaller multiplier.** This constant multiplies *every* caller's floor,
		// so it cannot come down for the expensive bodies without coming down
		// for the cheap one at the same time. The 7.74 us belongs to whoever
		// passes their own grain and says what their row costs.
		//
		// **The crossover is an amount of time, and this expresses it as a row
		// count.** Re-measured at `-O3`: three float adds per row cross near
		// 262,144 rows, and `physics::IntegrateMotion`'s `CFrame` step near
		// 8,000. Thirty-two times apart in rows; 49 us and 29 us in serial work,
		// which was one handover either way at the 31 us the pool then cost. A
		// row count can only be right for one row cost, and nothing in the
		// signature can know that cost - which is why both callers that measured
		// pass a grain of their own, and why `minimum` exists for the ones whose
		// index is not a row at all.
		//
		// **Both of those crossovers are owed a re-take and neither has had
		// one.** They were measured against a 31 us handover and it is 7.74 us
		// now, so the arithmetic says both should fall - perhaps to a quarter of
		// the serial work, which would be tens of thousands of rows for the cheap
		// body and a couple of thousand for the `CFrame` one. That is a
		// prediction and not a reading: `engine.ecs.bench.iteration` and
		// `engine.physics.bench.integrate` are where it gets settled, and until
		// they are re-run the constants stay where the last measurement put them.
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
		// unit of work is already large - a world tick is one index and tens of
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
	// off the frame's owning thread - locking there would put contention on every
	// span of every frame - so a world ticking on a worker contributes one
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
	// read with it on is a *serial* cost - useful for finding which stage is
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
