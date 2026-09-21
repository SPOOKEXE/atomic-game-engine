#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/World.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/WorldSystems.hpp>

TEST_SUITE_ID("client.world-systems")

TEST_CASE("an imported client world gets its configured physics rate before scripts", "[client][physics]") {
	engine::ecs::Store store("imported-world");
	engine::ecs::Scheduler systems;
	engine::world::WorldSettings settings;
	settings.PhysicsTickRate = 37.5;

	client::InstallClientWorldSystems(store, systems, settings.PhysicsTickRate);

	const auto *clock = engine::physics::PhysicsClockOf(store);
	REQUIRE(clock != nullptr);
	CHECK(clock->Rate == 37.5);
	CHECK(store.HasResource<engine::scene::Gravity>());
	CHECK(systems.HasSystem("physics.simulation", engine::ecs::Phase::Simulation));
	CHECK(systems.HasSystem("scene.gravity", engine::ecs::Phase::PreSimulation));
	CHECK(systems.HasSystem("scene.ownership", engine::ecs::Phase::PreSimulation));
}

TEST_CASE("client world setup preserves an imported physics clock", "[client][physics]") {
	engine::ecs::Store store("prepared-imported-world");
	engine::ecs::Scheduler systems;
	engine::physics::PreparePhysicsWorld(store);
	engine::physics::SetPhysicsTickRate(store, 120.0);
	store.ResourceMutable<engine::physics::PhysicsClock>()->Accumulator = 0.125;
	store.ResourceMutable<engine::physics::PhysicsClock>()->Steps = 17;
	engine::world::WorldSettings settings;
	settings.PhysicsTickRate = 37.5;

	client::InstallClientWorldSystems(store, systems, settings.PhysicsTickRate);

	const auto *clock = engine::physics::PhysicsClockOf(store);
	REQUIRE(clock != nullptr);
	CHECK(clock->Rate == 120.0);
	CHECK(clock->Accumulator == 0.125);
	CHECK(clock->Steps == 17);
}
