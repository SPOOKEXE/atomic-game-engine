#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Storm.hpp>
#include <engine/physics/Welds.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.physics.storm")
TEST_DEPENDS("engine.physics.pipeline")
TEST_DEPENDS("engine.physics.welds")
TEST_DEPENDS("engine.scene.storm")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace {
	constexpr float TICK = 1.0f / 60.0f;

	Entity DynamicPart(Store &store, const Vector3 &position, float mass) {
		engine::scene::PartDesc description;
		description.Frame = CFrame{position};
		description.Simulated = true;
		const Entity part = engine::scene::MakePart(store, description);
		store.GetMutable<engine::scene::RigidBody>(part)->Mass = mass;
		return part;
	}

	engine::physics::Storm StormAtOrigin() {
		engine::physics::Storm storm;
		storm.State.Parameters = engine::scene::EfPreset(engine::scene::EfCategory::EF3);
		storm.State.Parameters.TranslationVelocity = Vector3::Zero;
		storm.State.Parameters.Turbulence = 0.0f;
		return storm;
	}
}

TEST_CASE("storm drag uses authored area and rigid-body mass", "[physics][storm]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.storm.drag");
	engine::physics::PreparePhysicsWorld(store);
	const auto storm = StormAtOrigin();
	engine::physics::SetStorm(store, storm);

	const Entity body = DynamicPart(store, {storm.State.Parameters.CoreRadius, 0.0f, 0.0f}, 2.0f);
	engine::physics::StormResponse response;
	response.ExposedArea = 3.0f;
	response.DragCoefficient = 1.25f;
	store.Set(body, response);

	store.AdvanceTick(TICK);
	engine::physics::ApplyStormForces(store);

	const engine::physics::Storm *advanced = engine::physics::StormOf(store);
	REQUIRE(advanced != nullptr);
	const auto field = engine::scene::PrepareTornadoField(advanced->State.Parameters);
	const auto sample = engine::scene::SampleTornadoField(
		field,
		advanced->State.Position,
		store.Get<engine::scene::Transform>(body)->Frame.Position,
		advanced->State.ElapsedSeconds
	);
	const float speed = sample.Velocity.Magnitude();
	const float force = 0.5f * 1.225f * response.DragCoefficient * response.ExposedArea * speed * speed;
	const Vector3 expected = sample.Velocity.Unit() * (force * TICK / 2.0f);
	const Vector3 actual = store.Get<engine::scene::Motion>(body)->Linear;
	CHECK(actual.X == Approx(expected.X).margin(1e-5f));
	CHECK(actual.Y == Approx(expected.Y).margin(1e-5f));
	CHECK(actual.Z == Approx(expected.Z).margin(1e-5f));
}

TEST_CASE("storm link break disables the real weld and rebuilds connectivity", "[physics][storm]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.storm.links");
	const Entity workspace = engine::scene::InstallServices(store);
	engine::physics::PreparePhysicsWorld(store);
	const auto storm = StormAtOrigin();
	engine::physics::SetStorm(store, storm);
	const Entity first = DynamicPart(store, {storm.State.Parameters.CoreRadius, 0.0f, 0.0f}, 1.0f);
	const Entity second = DynamicPart(store, {storm.State.Parameters.CoreRadius + 1.0f, 0.0f, 0.0f}, 1.0f);
	store.SetParent(first, workspace);
	store.SetParent(second, workspace);
	store.Set(first, engine::physics::StormResponse{.ExposedArea = 2.0f});
	store.Set(second, engine::physics::StormResponse{.ExposedArea = 2.0f});

	const Entity link =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("WeldConstraint")), "StormLink");
	store.SetParent(link, first);
	store.Set(link, engine::scene::WeldConstraint{first, second});
	store.Set(link, engine::physics::StormLink{.BreakForce = 1.0f, .MaterialStrength = 1.0f});
	engine::physics::SolveRigidJoints(store);
	REQUIRE(store.Resource<engine::physics::PhysicsWorld>()->RigidlyConnected(first, second));

	store.AdvanceTick(TICK);
	engine::physics::ApplyStormForces(store);
	CHECK_FALSE(store.Get<engine::scene::WeldConstraint>(link)->Enabled);
	engine::physics::SolveRigidJoints(store);
	CHECK_FALSE(store.Resource<engine::physics::PhysicsWorld>()->RigidlyConnected(first, second));
}

TEST_CASE("storm state and force outcomes are deterministic fixtures", "[physics][storm]") {
	auto populate = [](Store &store) {
		engine::scene::RegisterSceneClasses();
		engine::physics::PreparePhysicsWorld(store);
		auto storm = StormAtOrigin();
		storm.State.Parameters.TranslationVelocity = {2.0f, 0.0f, -1.0f};
		engine::physics::SetStorm(store, storm);
		const Entity body = DynamicPart(store, {storm.State.Parameters.CoreRadius, 2.0f, 0.0f}, 3.0f);
		store.Set(body, engine::physics::StormResponse{.ExposedArea = 1.5f, .DragCoefficient = 0.9f});
		return body;
	};

	Store first("physics.storm.fixture.first");
	Store second("physics.storm.fixture.second");
	const Entity firstBody = populate(first);
	const Entity secondBody = populate(second);
	for (int tick = 0; tick < 20; ++tick) {
		first.AdvanceTick(TICK);
		second.AdvanceTick(TICK);
		engine::physics::ApplyStormForces(first);
		engine::physics::ApplyStormForces(second);
	}
	const auto *firstStorm = engine::physics::StormOf(first);
	const auto *secondStorm = engine::physics::StormOf(second);
	REQUIRE(firstStorm != nullptr);
	REQUIRE(secondStorm != nullptr);
	CHECK(firstStorm->State.Position == secondStorm->State.Position);
	CHECK(firstStorm->State.ElapsedSeconds == secondStorm->State.ElapsedSeconds);
	CHECK(
		first.Get<engine::scene::Motion>(firstBody)->Linear ==
		second.Get<engine::scene::Motion>(secondBody)->Linear
	);
}
