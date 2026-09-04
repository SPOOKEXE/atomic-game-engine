#include "WorldResource.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/physics/BodyMotion.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>

#include <cmath>

namespace engine::physics {

	namespace {
		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool CanSetVelocity(const ecs::Store &store, ecs::Entity body) {
			const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
			return rigidBody != nullptr && rigidBody->Kind != scene::BodyKind::Static &&
				   store.Has<scene::Simulated>(body);
		}

		bool Wake(ecs::Store &store, ecs::Entity body) {
			PhysicsWorld *world = PreparedWorldMutable(store);
			if (world == nullptr) {
				return false;
			}
			world->Wake(body);
			return true;
		}
	}

	core::Vector3 LinearVelocity(const ecs::Store &store, ecs::Entity body) {
		const scene::Motion *motion = store.Get<scene::Motion>(body);
		return motion == nullptr ? core::Vector3::Zero : motion->Linear;
	}

	core::Vector3 AngularVelocity(const ecs::Store &store, ecs::Entity body) {
		const scene::Motion *motion = store.Get<scene::Motion>(body);
		return motion == nullptr ? core::Vector3::Zero : motion->Angular;
	}

	bool SetLinearVelocity(ecs::Store &store, ecs::Entity body, const core::Vector3 &velocity) {
		if (!Finite(velocity) || !CanSetVelocity(store, body) || !Wake(store, body)) {
			return false;
		}

		const scene::Motion *previous = store.Get<scene::Motion>(body);
		store.Set<scene::Motion>(
			body, scene::Motion{velocity, previous == nullptr ? core::Vector3::Zero : previous->Angular}
		);
		return true;
	}

	bool SetAngularVelocity(ecs::Store &store, ecs::Entity body, const core::Vector3 &velocity) {
		if (!Finite(velocity) || !CanSetVelocity(store, body) || !Wake(store, body)) {
			return false;
		}

		const scene::Motion *previous = store.Get<scene::Motion>(body);
		store.Set<scene::Motion>(
			body, scene::Motion{previous == nullptr ? core::Vector3::Zero : previous->Linear, velocity}
		);
		return true;
	}

	bool ApplyImpulse(ecs::Store &store, ecs::Entity body, const core::Vector3 &impulse) {
		if (!Finite(impulse) || !CanSetVelocity(store, body)) {
			return false;
		}

		const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
		const scene::Collider *collider = store.Get<scene::Collider>(body);
		if (rigidBody == nullptr || rigidBody->Kind != scene::BodyKind::Dynamic || collider == nullptr) {
			return false;
		}

		const float mass = scene::MassOf(*collider, *rigidBody, store.Get<scene::PhysicsProperties>(body));
		if (!(mass > 0.0f) || !std::isfinite(mass)) {
			return false;
		}
		if (!Wake(store, body)) {
			return false;
		}

		const scene::Motion *previous = store.Get<scene::Motion>(body);
		const core::Vector3 linear = previous == nullptr ? core::Vector3::Zero : previous->Linear;
		const core::Vector3 angular = previous == nullptr ? core::Vector3::Zero : previous->Angular;
		store.Set<scene::Motion>(body, scene::Motion{linear + impulse / mass, angular});
		return true;
	}
}
