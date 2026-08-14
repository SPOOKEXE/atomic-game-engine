#include "Tasks.hpp"

#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cmath>

namespace engine::script {

	uint64_t TicksFor(const ecs::Store &store, double seconds) {
		const float delta = store.Time().Delta;

		// **Written as a *failed* `> 0` rather than as `<= 0`, so a NaN lands
		// here.** Every comparison against NaN is false, so `seconds <= 0.0`
		// let `task.wait(0/0)` through to a cast of NaN to `uint64_t`, which is
		// undefined behaviour reachable from a line of script.
		if (!(seconds > 0.0) || !(delta > 0.0f)) {
			return 1;
		}

		// Capped for the other end of the same hole: a `wait` of 1e300 seconds
		// overflows the cast as surely as a NaN does. The ceiling is a number of
		// *ticks* nothing will ever reach - about five million years at sixty
		// hertz - so a script asking for one is answered with "never" rather
		// than with whatever the conversion happened to produce.
		constexpr double NEVER = 1e16;

		const double ticks = std::ceil(seconds / static_cast<double>(delta));
		if (ticks < 1.0) {
			return 1;
		}
		return static_cast<uint64_t>(std::min(ticks, NEVER));
	}

	void TaskQueue::Delay(CallbackRef thread, uint64_t resumeAt) {
		const Wait wait{resumeAt, NextSequence++, thread};

		// **Inserted at the right place, not appended and re-sorted.** The list
		// is already ordered, so this is a binary search and a memmove;
		// `std::sort` over the whole vector on every insert made scheduling a
		// thousand waits cost `n log n` per call rather than `log n`, and the
		// benchmark measured 5.7 microseconds each for it.
		//
		// Kept ordered on insert rather than on drain so `Advance` is a walk of
		// a prefix - sorting per beat would pay on every tick nothing resumed,
		// and most ticks resume nothing.
		const auto at =
			std::upper_bound(Waiting.begin(), Waiting.end(), wait, [](const Wait &left, const Wait &right) {
				if (left.ResumeAt != right.ResumeAt) {
					return left.ResumeAt < right.ResumeAt;
				}
				// Ties break on scheduling order, which is what makes two
				// scripts waiting the same number of ticks resume in the order
				// they asked.
				return left.Sequence < right.Sequence;
			});
		Waiting.insert(at, wait);
	}

	void TaskQueue::Defer(CallbackRef thread) {
		Deferred.push_back(thread);
	}

	void TaskQueue::Advance(uint64_t tick, const std::function<void(CallbackRef)> &resume) {
		if (Waiting.empty() || Waiting.front().ResumeAt > tick) {
			return;
		}

		// Taken out of the queue **before** anything runs. A resumed script may
		// call `task.wait` again, and appending to the vector being walked would
		// resume it a second time in the same beat - which for the common
		// `while true do task.wait() end` is an infinite loop inside one tick.
		const auto due = std::find_if(Waiting.begin(), Waiting.end(), [tick](const Wait &wait) {
			return wait.ResumeAt > tick;
		});

		std::vector<Wait> running(Waiting.begin(), due);
		Waiting.erase(Waiting.begin(), due);

		for (const Wait &wait : running) {
			resume(wait.Thread);
		}
	}

	void TaskQueue::DrainDeferred(const std::function<void(CallbackRef)> &resume) {
		if (Deferred.empty()) {
			return;
		}

		// Swapped out for the reason `Advance` copies: a deferred thread that
		// defers again belongs to the next pass, and extending this one would
		// let a script spin the host without ever yielding a tick.
		std::vector<CallbackRef> running;
		running.swap(Deferred);

		for (const CallbackRef thread : running) {
			resume(thread);
		}
	}

	bool TaskQueue::Cancel(CallbackRef thread) {
		const auto found = std::find_if(Waiting.begin(), Waiting.end(), [thread](const Wait &wait) {
			return wait.Thread == thread;
		});

		if (found != Waiting.end()) {
			Waiting.erase(found);
			return true;
		}

		const auto deferred = std::find(Deferred.begin(), Deferred.end(), thread);
		if (deferred != Deferred.end()) {
			Deferred.erase(deferred);
			return true;
		}
		return false;
	}

	void TaskQueue::Clear(std::vector<CallbackRef> &released) {
		for (const Wait &wait : Waiting) {
			released.push_back(wait.Thread);
		}
		released.insert(released.end(), Deferred.begin(), Deferred.end());

		Waiting.clear();
		Deferred.clear();
	}
}
