#pragma once

// What to destroy, and which tick to destroy it on.
//
// **The shared half of `Debris`, beside `TaskQueue` and for its reasons.** A
// deadline names no VM, and what has to be one implementation is the order two
// items due on the same tick are destroyed in — a script that spawns an effect
// and its shell together should see them go in the order it added them, on
// every run of the recording.
//
// **The unit is a tick and the argument is seconds**, which is exactly what
// `docs/retired/SCRIPT_CONCURRENCY.md` §2 settled for `task.wait` and is settled
// again here rather than differently: seconds because that is what an author
// means and what Roblox takes, ticks because a deadline measured against a wall
// clock arrives after a different amount of *simulation* on a busy machine than
// on an idle one. `Debris:AddItem(part, 0.5)` is thirty ticks at sixty hertz on
// every machine that ever runs it.
//
// **A cap, and the failure it takes is deliberate.** `AddItem` is a *cleanup*
// call, so the conservative failure when a script fills the queue from a loop is
// to destroy the oldest item early — the thing was going to be destroyed anyway
// and the only cost is that it went sooner. That is the opposite trade from
// `TweenTable::MAXIMUM`, which refuses rather than evicting, and the difference
// is the direction each is wrong in: a tween silently dropped is a scene that
// animates on a small world and not on a big one, where a debris item dropped
// early is a scene that tidies up sooner.
//
// **No `MaxItems` property, unlike Roblox.** A cap a script can raise is not a
// bound, and the property would make `Debris` the second service in this engine
// that has to be a userdata rather than a table — see `ServiceSurface::Index`
// for what that costs and why.
//
// @tier L9 · shared

#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::script {

	// What is waiting to be destroyed, and when.
	//
	// One per VM, beside `SignalTable` and `TaskQueue`. Holds no VM types: what
	// it produces is a list of entities, and the binding destroys them.
	//
	// @since v0.16
	class DebrisQueue {
	  public:
		// How many items may be waiting at once. Roblox's default.
		static constexpr size_t MAXIMUM = 1000;

		// Schedules an instance for destruction.
		//
		// **Adding the same instance twice keeps the *earlier* deadline** and
		// does not queue a second entry. A script calling `AddItem` in a loop on
		// something it is already counting down would otherwise fill the queue
		// with one instance, and the second destruction of an entity is a no-op
		// anyway — so the entry would only ever have been dead weight.
		//
		// @param instance What to destroy.
		// @param dueTick  The tick it becomes due on.
		// @return The instance evicted to make room, or `NULL_ENTITY`. The
		//         caller destroys it, which is what "evicted" means here.
		ecs::Entity Add(ecs::Entity instance, uint64_t dueTick);

		// Hands over everything due at or before `tick` and forgets it.
		//
		// **Ties break on the order they were added**, which is what makes two
		// items with one deadline go in the order the script asked. A heap
		// ordered on the tick alone would leave that to whichever way the
		// comparison happened to fall — the same rule and the same reason as
		// `TaskQueue::Advance`.
		//
		// @param tick    The tick the world has reached.
		// @param expired Appended, in order, with everything due.
		void Advance(uint64_t tick, std::vector<ecs::Entity> &expired);

		// **There is no `Forget`, and an instance destroyed early needs none.**
		// Its entry comes due, `Store::DestroyInstance` on a row that has
		// already gone is a no-op, and the entry leaves the queue exactly as it
		// would have. A second removal path would be a second place the queue
		// can be got wrong for no behaviour that is not already correct.

		// How many items are waiting.
		size_t Count() const {
			return Items.size();
		}

		// **There is no `Clear` either.** A world being torn down takes these
		// with it, and a queue holding entity handles into a store that has gone
		// is not something anything reads — the same reason `TaskQueue` is not
		// emptied when a runtime closes.

	  private:
		// One item waiting.
		struct Item {
			// The tick it becomes due on.
			uint64_t DueTick = 0;

			// Where it sat in insertion order, so ties break deterministically
			// and eviction takes the genuinely oldest rather than whichever the
			// sort happened to leave first.
			uint64_t Sequence = 0;

			ecs::Entity Instance;
		};

		// Sorted on `(DueTick, Sequence)`. A vector rather than a heap, for
		// `TaskQueue::Waiting`'s reason: the sort is what makes the order stated
		// rather than incidental, and a heap's tie order is an implementation
		// detail no recording should depend on.
		std::vector<Item> Items;
		uint64_t NextSequence = 1;
	};

	// Destroys everything whose deadline the world has reached.
	//
	// **A store and a queue rather than a VM, so both runtimes call this one.**
	// Draining debris destroys instances and fires nothing, so there is no
	// callable for a language to know how to call — which is the whole of why
	// `PumpTweens` is still two functions and this is one. It was two identical
	// ten-line functions until v0.16.
	//
	// Runs at the head of the barrier, immediately after the tweens, for the
	// reason `LuauRuntime::Heartbeat` gives: an instance's last tick of motion
	// happens before it is taken away.
	//
	// @param store The world.
	// @param queue That VM's queue.
	// @since v0.16
	void PumpDebris(ecs::Store &store, DebrisQueue &queue);
}
