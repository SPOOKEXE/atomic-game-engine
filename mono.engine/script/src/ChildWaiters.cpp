#include <engine/ecs/Store.hpp>
#include <engine/script/ChildWaiters.hpp>

#include <utility>

namespace engine::script {

	uint64_t ChildWaiters::Add(ecs::Entity parent, std::string name, uint64_t dueTick) {
		if (Waits.size() >= MAXIMUM) {
			return 0;
		}

		const uint64_t id = NextSequence++;
		Waits.push_back(Wait{parent, std::move(name), dueTick, id});
		return id;
	}

	void ChildWaiters::Advance(const ecs::Store &store, uint64_t tick, std::vector<Resumption> &ready) {
		if (Waits.empty()) {
			return;
		}

		// Rebuilt rather than erased through, so one pass decides every waiter
		// and the survivors keep their insertion order.
		std::vector<Wait> waiting;
		waiting.reserve(Waits.size());

		for (Wait &wait : Waits) {
			// **The parent first, because a dead row cannot answer either
			// question.** `FindFirstChild` on a destroyed instance is a walk of
			// nothing, and the honest answer to "wait for a child of this" when
			// "this" has gone is nothing, now.
			if (!store.Alive(wait.Parent)) {
				ready.push_back(Resumption{wait.Sequence, ecs::NULL_ENTITY});
				continue;
			}

			// Non-recursive, which is `WaitForChild`'s own shape: Roblox's takes
			// no recursive flag, and a wait that matched a grandchild would
			// answer with something `FindFirstChild(name)` never would.
			if (const ecs::Entity child = store.FindFirstChild(wait.Parent, wait.Name);
				child != ecs::NULL_ENTITY) {
				ready.push_back(Resumption{wait.Sequence, child});
				continue;
			}

			// **At or past, rather than past.** The deadline is the tick the
			// wait gives up *on*, so a one-tick wait made on tick five is
			// answered on tick six and never on tick seven - which is what makes
			// `TicksFor`'s "at least one tick, never zero" mean one beat of
			// waiting rather than one or two depending on where in the barrier
			// the call happened to land.
			if (tick >= wait.DueTick) {
				ready.push_back(Resumption{wait.Sequence, ecs::NULL_ENTITY});
				continue;
			}

			waiting.push_back(std::move(wait));
		}

		Waits = std::move(waiting);
	}
}
