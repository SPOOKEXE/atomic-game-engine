#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>

namespace engine::scene {

	void RegisterGravitySystem(ecs::Scheduler &scheduler) {
		scheduler.Add(
			"scene.gravity",
			ecs::Phase::PreSimulation,
			[](ecs::Store &store) {
				const Gravity *gravity = store.Resource<Gravity>();
				if (gravity == nullptr) {
					return;
				}

				// **Read once, outside the walk.** A resource lookup per row is a
				// hash per body per tick to fetch three floats that cannot change
				// while the walk runs.
				const core::Vector3 acceleration = gravity->Acceleration;
				const float delta = store.Time().Delta;

				// **`Simulated` is named even though `Motion` already implies it**,
				// because it no longer follows from `RigidBody`: every part carries
				// one of those now, so the term that means "the world may move this"
				// has to be written down. A static part has no `Motion` either, so
				// the set is the same one - and saying so keeps it the same one the
				// day something hands a `Motion` to a row that should not have had
				// it.
				//
				// A positive term since v0.18, where it was a `Without` before. The
				// ECS matches archetypes on the terms a query names, so this costs
				// nothing where the exclusion cost a test per table per plan.
				//
				// `With` rather than naming it in the `Query<>` row, because it is a
				// tag: there is nothing to hand the callback, and a parameter for an
				// empty struct would be a parameter every reader has to look up.
				store.Query<Motion, const RigidBody>().With<Simulated>().Each(
					[acceleration, delta](ecs::Entity, Motion &motion, const RigidBody &body) {
						// Dynamic only. A static or kinematic body is moved by
						// whatever owns it, and accelerating it here would fight
						// that owner rather than adding weight to it.
						if (body.Kind != BodyKind::Dynamic) {
							return;
						}
						motion.Linear = motion.Linear + acceleration * delta;
					}
				);
			},
			ecs::SystemOrder{{}, {}, {}, {"character.control"}}
		);
	}

	void PrepareGravity(ecs::Store &store) {
		// **Not overwritten**, so a host may call this on every world it opens
		// without undoing what a game file authored. The default is what a world
		// gets for saying nothing, which is the only thing a default should be.
		if (!store.HasResource<Gravity>()) {
			store.SetResource(Gravity{});
		}
	}
}
