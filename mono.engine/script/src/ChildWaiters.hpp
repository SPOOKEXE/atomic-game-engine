#pragma once

// Who is waiting for which child, and until which tick.
//
// **The shared half of `WaitForChild`, beside `DebrisQueue` and for its
// reasons.** What a script is waiting for — a parent, a name and a deadline —
// names no VM, and what has to be one implementation is the order two waiters
// answered on the same tick resume in: a script that waits for two children of
// one parent should see them arrive in the order it asked, on every run of the
// recording.
//
// **The unit is a tick and the argument is seconds**, which is `Debris.hpp`'s
// rule and `task.wait`'s, reached through the same `TicksFor`. A timeout
// measured against a wall clock expires after a different amount of *simulation*
// on a busy machine than on an idle one, so `WaitForChild(name, 0.5)` gives up
// after thirty ticks at sixty hertz on every machine that ever runs it.
//
// ## Answered from the store rather than from the arrival list
//
// `PumpTree` walks `ecs::TreeChange` at the barrier and a waiter could have been
// a filter over that walk. It is a lookup against the store instead, and the
// difference is three refusals rather than a preference:
//
// - **`TakeTreeChanges` is a *take*.** Whichever pump drains it first empties
//   it, so a second consumer of that list would be a second claim on one
//   swap — and the tree pump is per language, where this is not.
// - **A tree change is only recorded once something calls `ObserveTree`**, which
//   is opt-in with no way back off. A `WaitForChild` in a world where nothing
//   connected a tree signal would wait for a list nobody is filling, and making
//   the method turn observation on would charge every such world an archetype
//   move for the life of the world.
// - **A child renamed *into* the awaited name is an arrival as far as the author
//   is concerned**, and it is not a reparent, so it produces no `TreeChange` at
//   all. Asking the store is the question the script actually asked.
//
// The cost is a sibling walk per waiter per beat, and it is paid only while
// something is waiting — `Empty` is the guard, and the ordinary state of a world
// is that nothing is.
//
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {
	namespace ecs {
		class Store;
	}
}

namespace engine::script {

	// Every `WaitForChild` this VM has outstanding.
	//
	// One per VM, beside `SignalTable`, `TaskQueue` and `DebrisQueue`. Holds no
	// VM types: what it produces is an id and an entity, and the binding turns
	// those into a resume.
	//
	// @since v0.15
	class ChildWaiters {
	  public:
		// How many waits may be outstanding at once.
		//
		// **Chosen rather than derived, and it bounds suspended scripts rather
		// than bytes.** Every entry here is a coroutine or a promise the VM is
		// holding alive, so a world with hundreds of them waiting on one tick is
		// a world doing something the engine should be saying out loud about.
		static constexpr size_t MAXIMUM = 256;

		// What one waiter is owed.
		struct Resumption {
			// The id `Add` handed back, which is what the VM keyed its
			// suspended script on.
			uint64_t Waiter = 0;

			// The child that arrived, or `NULL_ENTITY` for a deadline that
			// passed. A waiter is answered exactly once either way.
			ecs::Entity Child;
		};

		// Registers a wait.
		//
		// **Refuses rather than evicting when full, which is the opposite trade
		// from `DebrisQueue` and the same one as `TweenTable`.** Dropping the
		// oldest waiter would resume a script with nil for a child that was
		// about to arrive — a wrong answer, silently, in the one case the method
		// exists for — where refusing is an error at the call site of the script
		// that filled the queue.
		//
		// @param parent  The instance whose children to watch.
		// @param name    The child's name, as the script spelled it.
		// @param dueTick The tick the wait gives up on.
		// @return The waiter's id, or zero when the queue is full.
		uint64_t Add(ecs::Entity parent, std::string name, uint64_t dueTick);

		// Answers every waiter that can be answered at this tick, oldest first.
		//
		// **The store rather than the arrival list**, for the three reasons the
		// header gives. A waiter whose parent has been destroyed is answered
		// with nothing immediately rather than at its deadline: the child can
		// never arrive under a row that has gone, and a script left suspended
		// for a timeout it can no longer be waiting on is a tick of latency
		// bought with nothing.
		//
		// **Taken out of the queue before anything is resumed**, which is
		// `TaskQueue::Advance`'s rule: a resumed script may wait again, and
		// appending to the vector being walked would answer it twice in one
		// beat.
		//
		// @param store The world.
		// @param tick  The tick the world has reached.
		// @param ready Appended, in the order the waits were made.
		void Advance(const ecs::Store &store, uint64_t tick, std::vector<Resumption> &ready);

		// Whether anything is waiting.
		//
		// @return `true` when `Advance` would look at nothing.
		bool Empty() const {
			return Waits.empty();
		}

		// How many waits are outstanding.
		size_t Count() const {
			return Waits.size();
		}

		// **There is no `Cancel`, and nothing asks for one.** A script cannot
		// abandon a `WaitForChild` — it is suspended inside the call — and a
		// world being torn down takes the VM's threads with it, which is
		// `DebrisQueue`'s reason for having no `Clear`.

	  private:
		// One outstanding wait.
		struct Wait {
			ecs::Entity Parent;

			// The name as the script spelled it, rather than a `core::Name`.
			// Interning is process-wide and permanent, and a script may wait for
			// a child that never arrives — so a typo would otherwise leave a row
			// in the registry for the life of the process.
			std::string Name;

			// The tick the wait gives up on.
			uint64_t DueTick = 0;

			// Also the waiter's id, so insertion order and identity are one
			// number rather than two that have to agree.
			uint64_t Sequence = 0;
		};

		// In insertion order, which is answer order. A vector rather than a map
		// keyed on `(parent, name)`: two scripts may wait for the same child and
		// both are answered, and the walk is bounded by `MAXIMUM`.
		std::vector<Wait> Waits;

		// Starts at one, leaving zero to mean "no waiter" — the same convention
		// `LuauContext::NextHostCallback` uses.
		uint64_t NextSequence = 1;
	};
}
