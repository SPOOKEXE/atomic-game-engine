#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Ownership.hpp>

#include <client/WorldSystems.hpp>

namespace client {
	void InstallClientWorldSystems(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &systems,
		std::optional<double> initialPhysicsTickRate
	) {
		if (engine::physics::PhysicsClockOf(store) == nullptr) {
			engine::physics::PreparePhysicsWorld(store);
			if (initialPhysicsTickRate) {
				engine::physics::SetPhysicsTickRate(store, *initialPhysicsTickRate);
			}
		}
		engine::physics::RegisterPhysicsSystems(systems);
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(systems);
		engine::scene::RegisterOwnershipSystem(systems);
	}
}
