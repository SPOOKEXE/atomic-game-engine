#include "Subtree.hpp"

#include <algorithm>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::Store;
	}

	void EachDescendant(const Store &store, Entity instance, const std::function<void(Entity)> &body) {
		// A stack, popped from the back. The previous shape of this walk kept a
		// vector and took the front of it, which made every pop and every insert
		// an O(n) shift of the whole pending list — quadratic over a subtree,
		// on the call a script uses to find things.
		std::vector<Entity> pending;

		// Children go on reversed, so the first child is the next one off the
		// back. That is what keeps this in the recursive walk's order while
		// popping from the cheap end.
		const auto push = [&](Entity parent) {
			const size_t mark = pending.size();
			store.EachChild(parent, [&](Entity child) { pending.push_back(child); });
			std::reverse(pending.begin() + static_cast<ptrdiff_t>(mark), pending.end());
		};

		push(instance);
		while (!pending.empty()) {
			const Entity current = pending.back();
			pending.pop_back();

			body(current);
			push(current);
		}
	}

	void ForgetSubtree(
		const Store &store,
		SignalTable &signals,
		ChangeQueue &changes,
		Entity instance,
		const std::function<void(CallbackRef)> &release
	) {
		// One vector for the whole subtree. `DropSubject` appends, so the
		// releases can happen once at the end rather than per row, and the
		// order they come back in is the order they were connected.
		std::vector<CallbackRef> released;

		const auto forget = [&](Entity subject) {
			signals.DropSubject(subject, released);
			changes.Unwatch(subject);
		};

		forget(instance);
		EachDescendant(store, instance, forget);

		for (const CallbackRef reference : released) {
			release(reference);
		}
	}
}
