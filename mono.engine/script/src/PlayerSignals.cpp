// Whether a tree change is a player arriving or a character leaving.
//
// **Two predicates over the class table, and nothing about either is per
// language.** They lived in `LuauInstances.cpp` until v0.19, which was fine
// while both VMs were one library and stopped being fine when they became two:
// `JsSurface.cpp` calls `IsPlayerOfService` on the same two tree signals, and an
// adapter may not reach into the other adapter's object files.
//
// `Signals.hpp` is where both are declared, and this is the file it names.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Signals.hpp>

namespace engine::script {

	namespace {
		// The `Players` service's class, resolved once.
		//
		// A `ClassId` is a registration index and registration is process-wide
		// and idempotent, so this is stable for the life of the process - which
		// is what lets the test below be two integer compares rather than a
		// lookup per tree change.
		ecs::ClassId PlayersClass() {
			static const ecs::ClassId id = [] {
				scene::ServiceClass();
				return ecs::Classes::Find(core::Name("Players"));
			}();
			return id;
		}
	}

	bool IsPlayerOfService(const ecs::Store &store, ecs::Entity container, ecs::Entity instance) {
		if (container == ecs::NULL_ENTITY || instance == ecs::NULL_ENTITY) {
			return false;
		}
		return ecs::Classes::IsA(store.ClassOf(container), PlayersClass()) &&
			   ecs::Classes::IsA(store.ClassOf(instance), scene::PlayerClass());
	}

	ecs::Entity PlayerLosingCharacter(const ecs::Store &store, ecs::Entity container, ecs::Entity instance) {
		if (container == ecs::NULL_ENTITY || instance == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		// The nearest ancestor only - see the declaration for why the gate is
		// what makes this fire once rather than once per level.
		if (store.ParentOf(instance) != container) {
			return ecs::NULL_ENTITY;
		}
		return scene::PlayerOf(store, instance);
	}

}
