#include "Inertia.hpp"
#include "WorldResource.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/physics/BodyMotion.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>

#include <array>
#include <cmath>
#include <vector>

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

		bool CanApplyLoad(const ecs::Store &store, ecs::Entity body) {
			const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
			const scene::Collider *collider = store.Get<scene::Collider>(body);
			if (rigidBody == nullptr || collider == nullptr || rigidBody->Kind != scene::BodyKind::Dynamic ||
				!store.Has<scene::Simulated>(body)) {
				return false;
			}
			const float mass =
				scene::MassOf(*collider, *rigidBody, store.Get<scene::PhysicsProperties>(body));
			return mass > 0.0f && std::isfinite(mass);
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

	core::Vector3 AppliedForce(const ecs::Store &store, ecs::Entity body) {
		const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
		return rigidBody == nullptr ? core::Vector3::Zero : rigidBody->AppliedForce;
	}

	core::Vector3 AppliedTorque(const ecs::Store &store, ecs::Entity body) {
		const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
		return rigidBody == nullptr ? core::Vector3::Zero : rigidBody->AppliedTorque;
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

	bool SetAppliedForce(ecs::Store &store, ecs::Entity body, const core::Vector3 &force) {
		if (!Finite(force) || !CanApplyLoad(store, body) ||
			(force != core::Vector3::Zero && !Wake(store, body))) {
			return false;
		}
		store.GetMutable<scene::RigidBody>(body)->AppliedForce = force;
		return true;
	}

	bool SetAppliedTorque(ecs::Store &store, ecs::Entity body, const core::Vector3 &torque) {
		if (!Finite(torque) || !CanApplyLoad(store, body) ||
			(torque != core::Vector3::Zero && !Wake(store, body))) {
			return false;
		}
		store.GetMutable<scene::RigidBody>(body)->AppliedTorque = torque;
		return true;
	}

	void ApplyPersistentLoads(ecs::Store &store) {
		const float delta = PhysicsStepSeconds(store);
		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Sleep removes `Motion` as the archetype move that takes a body out of
		// the dynamic set. An active persistent load is an instruction to keep
		// simulating, so restore that row before this step's integration. The
		// structural writes stay outside the query that discovers them.
		std::vector<ecs::Entity> loadedSleepers;
		store.Query<const scene::Collider, const scene::RigidBody>().With<scene::Simulated>().Each(
			[&](ecs::Entity entity, const scene::Collider &, const scene::RigidBody &body) {
				if (body.Kind != scene::BodyKind::Dynamic ||
					(body.AppliedForce == core::Vector3::Zero && body.AppliedTorque == core::Vector3::Zero)) {
					return;
				}
				world->Wake(entity);
				if (!store.Has<scene::Motion>(entity)) {
					loadedSleepers.push_back(entity);
				}
			}
		);
		for (const ecs::Entity entity : loadedSleepers) {
			store.Set<scene::Motion>(entity, scene::Motion{});
		}

		store.Query<scene::Motion, const scene::Transform, const scene::Collider, const scene::RigidBody>()
			.With<scene::Simulated>()
			.Each([&store, delta](
					  ecs::Entity entity,
					  scene::Motion &motion,
					  const scene::Transform &transform,
					  const scene::Collider &collider,
					  const scene::RigidBody &body
				  ) {
				if (body.Kind != scene::BodyKind::Dynamic) {
					return;
				}
				const float mass = scene::MassOf(collider, body, store.Get<scene::PhysicsProperties>(entity));
				if (!(mass > 0.0f) || !std::isfinite(mass)) {
					return;
				}
				const std::array<core::Vector3, 3> axes{
					transform.Frame.RightVector(),
					transform.Frame.UpVector(),
					transform.Frame.VectorToWorldSpace(core::Vector3::ZAxis),
				};
				motion.Linear = motion.Linear + body.AppliedForce * (delta / mass);
				motion.Angular =
					motion.Angular +
					AngularAcceleration(axes, InverseInertiaOf(collider, mass), body.AppliedTorque) * delta;
			});
	}
}
