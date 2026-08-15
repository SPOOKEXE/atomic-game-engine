#include "WorldResource.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>

namespace engine::physics {

	namespace {
		// Interned once. `core::Name` from a literal is a hash of the text, and
		// this is asked several times a tick and once per query; a
		// function-local static makes every call after the first an integer
		// compare, and the language guarantees the initialisation is
		// thread-safe.
		core::Name ComponentName() {
			static const core::Name name{PHYSICS_WORLD_COMPONENT};
			return name;
		}

		// Loud rather than silent, and every tick rather than once: a world
		// with no `PhysicsWorld` produces no pairs, no contacts and no query
		// answers at all, so one line at startup would scroll away long before
		// anybody wondered why nothing collides.
		void Complain(const ecs::Store &store) {
			ENGINE_ERROR(
				"store '{}': physics has no PhysicsWorld resource. Call PreparePhysicsWorld.", store.Name()
			);
		}

		// Whether the typed lookup below is safe to make.
		//
		// **The order is the whole point.** A world can only hold the resource
		// if something registered the type first, so an unregistered type is a
		// complete answer to "is this world prepared" - and it is an answer
		// reached by a name lookup rather than by the typed one, which would
		// register the type to tell us it was missing.
		bool Prepared(const ecs::Store &store) {
			if (PhysicsWorldRegistered()) {
				return true;
			}
			Complain(store);
			return false;
		}
	}

	bool PhysicsWorldRegistered() {
		return ecs::Components::Find(ComponentName()).IsValid();
	}

	const PhysicsWorld *PreparedWorld(const ecs::Store &store) {
		if (!Prepared(store)) {
			return nullptr;
		}

		// Safe now: the type is registered under the explicit name, so the
		// `Components::Of` inside this call adopts that registration instead of
		// minting one.
		const PhysicsWorld *world = store.Resource<PhysicsWorld>();
		if (world == nullptr) {
			Complain(store);
		}
		return world;
	}

	PhysicsWorld *PreparedWorldMutable(ecs::Store &store) {
		if (!Prepared(store)) {
			return nullptr;
		}

		PhysicsWorld *world = store.ResourceMutable<PhysicsWorld>();
		if (world == nullptr) {
			Complain(store);
		}
		return world;
	}
}
