#pragma once

// When a suspended script may run again.
//
// **`docs/retired/SCRIPT_CONCURRENCY.md` §1 is the whole specification**: a script may
// only resume from something the barrier delivers in a deterministic order, and
// a tick boundary is one of the three legal sources. So every queue here is
// keyed on a *tick number*, never on a clock, and `Advance` is called once per
// beat with the tick the world has reached.
//
// §2 settled the spelling and the unit, and both are refusals rather than
// conveniences:
//
// - **`task.wait(n)` takes seconds and rounds to ticks**, reported in the
//   documentation and in the declaration files. Seconds because that is what an
//   author means and what Roblox takes; ticks because a wall-clock sleep resumes
//   after a different amount of simulation on a busy machine than on an idle
//   one, which is the desync rule 5 names.
// - **bare `wait` does not exist**, and asking for it is an error naming its
//   replacement. §2's recommendation, and the reason is that a familiar name
//   with different semantics costs a debugging session while a refusal costs one
//   lookup.
//
// **The threads live in the VM and the schedule lives here**, exactly as
// `SignalTable` splits them. A Luau coroutine and a JavaScript continuation have
// nothing in common except that each VM can name one with an integer.
//
// @tier L9 · shared

#include "Signals.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace engine::script {

	// Resumes waiting for a deterministic moment.
	//
	// @since v0.6
	class TaskQueue {
	  public:
		// Schedules a resume for a future tick.
		//
		// @param thread    The VM's name for the suspended thread.
		// @param resumeAt  The tick to resume on.
		void Delay(CallbackRef thread, uint64_t resumeAt);

		// Schedules a resume for the end of this beat.
		//
		// `task.defer`. Later than `task.spawn` and earlier than the next tick,
		// which is a real distinction: deferred work sees everything this beat
		// did, and a resume next tick would see one more tick of simulation.
		//
		// @param thread The VM's name for the suspended thread.
		void Defer(CallbackRef thread);

		// Resumes everything due at or before `tick`, oldest first.
		//
		// **Ties break on scheduling order**, which is what makes two scripts
		// waiting the same number of ticks resume in the order they asked. A
		// heap ordered on the tick alone would leave that to whichever way the
		// comparison happened to fall.
		//
		// @param tick   The tick the world has reached.
		// @param resume Called with each thread to run.
		void Advance(uint64_t tick, const std::function<void(CallbackRef)> &resume);

		// Resumes everything deferred, in order, and empties the list.
		//
		// A deferred thread that defers again lands in the *next* pass rather
		// than extending this one, for the reason `SignalTable::Fire`
		// snapshots its count: otherwise a script could spin the host without
		// ever yielding a tick.
		//
		// @param resume Called with each thread to run.
		void DrainDeferred(const std::function<void(CallbackRef)> &resume);

		// Forgets a scheduled resume.
		//
		// `task.cancel`. The thread is **not** released here: only the VM knows
		// how, and the caller releases what this hands back.
		//
		// @param thread The thread to unschedule.
		// @return `true` when something was scheduled for it.
		bool Cancel(CallbackRef thread);

		// How many resumes are outstanding.
		//
		// @return The count of scheduled and deferred threads.
		size_t Pending() const {
			return Waiting.size() + Deferred.size();
		}

		// Empties both queues and reports every thread to release.
		//
		// @param released Appended with every thread the queue held.
		void Clear(std::vector<CallbackRef> &released);

	  private:
		// One scheduled resume.
		struct Wait {
			// The tick it becomes due on.
			uint64_t ResumeAt = 0;

			// Where it sat in scheduling order, so ties break deterministically.
			uint64_t Sequence = 0;

			// The VM's name for the thread.
			CallbackRef Thread = 0;
		};

		// Sorted on `(ResumeAt, Sequence)`. A vector rather than a heap: waits
		// are few, the sort is what makes the order stated rather than
		// incidental, and a heap's tie order is an implementation detail no
		// recording should depend on.
		std::vector<Wait> Waiting;
		std::vector<CallbackRef> Deferred;
		uint64_t NextSequence = 1;
	};
}
