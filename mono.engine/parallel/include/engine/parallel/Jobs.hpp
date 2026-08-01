#pragma once

// Engine-internal blocking dispatch over one process-wide worker pool.
//
// This is not the userland `thread` datatype or process dispatch. Jobs are
// fork-joined inside the call that starts them and cannot outlive that call.
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
		// Only one For may be active process-wide. Concurrent calls, including a
		// nested For from `body`, are unsupported and can deadlock. If a pooled
		// invocation throws, the failed range is abandoned, the other ranges
		// finish, and the first captured exception is rethrown on the caller. An
		// inline invocation rethrows directly.
		//
		// @param count Number of indices in the half-open span `[0, count)`.
		// @param grain Maximum indices per pooled range and the inline cutoff, or
		//              zero for DEFAULT_GRAIN.
		// @param body  Callable given each half-open range `[begin, end)`; shared
		//              captures must permit concurrent access.
		// @tick
		static void For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body);

		// Default pooled range size for cheap per-index work.
		//
		// Measured with a three-multiply-add ECS integration step. Expensive work
		// should pass a smaller grain selected from its own release-build profile.
		static constexpr size_t DEFAULT_GRAIN = 4096;
	};
}
