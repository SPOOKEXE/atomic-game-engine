#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>

namespace engine::scene {

	void RegisterGravitySystem(ecs::Scheduler &scheduler) {
		scheduler.Add("scene.gravity", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			const Gravity *gravity = store.Resource<Gravity>();
			if (gravity == nullptr) {
				return;
			}

			// **Read once, outside the walk.** A resource lookup per row is a
			// hash per body per tick to fetch three floats that cannot change
			// while the walk runs.
			const core::Vector3 acceleration = gravity->Acceleration;
			const float delta = store.Time().Delta;

			store.Each<Motion, const RigidBody>([acceleration,
												 delta](ecs::Entity, Motion &motion, const RigidBody &body) {
				// Dynamic only. A static or kinematic body is moved by
				// whatever owns it, and accelerating it here would fight
				// that owner rather than adding weight to it.
				if (body.Kind != BodyKind::Dynamic) {
					return;
				}
				motion.Linear = motion.Linear + acceleration * delta;
			});
		});
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
