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
#include <functional>

namespace engine::parallel {

	// Dispatches one blocking parallel-for batch across the caller and a worker pool.
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
		static void For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body);

		// Default pooled range size for cheap per-index work.
		//
		// Measured with a three-multiply-add ECS integration step. Expensive work
		// should pass a smaller grain selected from its own release-build profile.
		static constexpr size_t DEFAULT_GRAIN = 4096;
	};
}
