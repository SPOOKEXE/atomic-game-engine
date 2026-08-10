#include <engine/ecs/Store.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Components.hpp>

namespace engine::scene {

	bool KeepWorldAwake(ecs::Store &store, ecs::Entity instance, core::Name reason) {
		if (!store.Alive(instance)) {
			return false;
		}
		store.Set<AwakeWorld>(instance, AwakeWorld{reason});
		return true;
	}

	void LetWorldSleep(ecs::Store &store, ecs::Entity instance) {
		store.Remove<AwakeWorld>(instance);
	}

	bool HoldsWorldAwake(const ecs::Store &store, ecs::Entity instance) {
		return store.Get<AwakeWorld>(instance) != nullptr;
	}

	bool WorldIsHeldAwake(ecs::Store &store, core::Name *reason) {
		// **Stops at the first row.** A host asks this once per world per tick
		// and the answer is a yes or a no; walking the rest to count them would
		// be work done for a number nobody reads.
		bool held = false;
		store.Each<const AwakeWorld>([&held, reason](ecs::Entity, const AwakeWorld &awake) {
			if (held) {
				return;
			}
			held = true;
			if (reason != nullptr) {
				*reason = awake.Reason;
			}
		});
		return held;
	}
}
