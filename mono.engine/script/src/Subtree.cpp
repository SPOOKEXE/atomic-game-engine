#include <engine/script/Subtree.hpp>

#include <algorithm>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::Store;
	}

	void EachDescendant(const Store &store, Entity instance, const std::function<void(Entity)> &body) {
		// **`Store` owns the walk now, and this is the caller the header
		// predicted.** `Subtree.hpp` said the walk lived here because only this
		// module needed one, and that if a second module ever wanted it, it
		// belonged on `Store` and this became a caller. `ecs`'s own recursive
		// lookups - `FindFirstChild(name, true)` and
		// `FindFirstChildWhichIsA` - are that second module, and a descendant
		// order defined in two places is two orders the day one is changed.
		store.EachDescendant(instance, body);
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
