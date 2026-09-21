#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/BodyMotion.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.physics.bodymotion")
TEST_DEPENDS("engine.scene.part")
TEST_DEPENDS("engine.physics.pipeline")

using Catch::Approx;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace {
	Entity DynamicPart(Store &store, float mass = 1.0f) {
		engine::scene::PartDesc description;
		description.Simulated = true;
		const Entity part = engine::scene::MakePart(store, description);
		store.GetMutable<engine::scene::RigidBody>(part)->Mass = mass;
		return part;
	}
}

TEST_CASE("velocity setters wake and retain the other body velocity", "[physics][bodymotion]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.velocity");
	engine::physics::PreparePhysicsWorld(store);
	const Entity part = DynamicPart(store);

	REQUIRE(engine::physics::SetLinearVelocity(store, part, Vector3{3.0f, -2.0f, 1.0f}));
	CHECK(engine::physics::LinearVelocity(store, part) == Vector3{3.0f, -2.0f, 1.0f});
	CHECK(engine::physics::AngularVelocity(store, part) == Vector3::Zero);

	REQUIRE(engine::physics::SetAngularVelocity(store, part, Vector3{0.0f, 2.5f, -1.0f}));
	CHECK(engine::physics::LinearVelocity(store, part) == Vector3{3.0f, -2.0f, 1.0f});
	CHECK(engine::physics::AngularVelocity(store, part) == Vector3{0.0f, 2.5f, -1.0f});
}

TEST_CASE("an impulse uses the same mass as the solver", "[physics][bodymotion]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.impulse");
	engine::physics::PreparePhysicsWorld(store);
	const Entity part = DynamicPart(store, 4.0f);

	REQUIRE(engine::physics::ApplyImpulse(store, part, Vector3{8.0f, -4.0f, 2.0f}));
	const Vector3 velocity = engine::physics::LinearVelocity(store, part);
	CHECK(velocity.X == Approx(2.0f));
	CHECK(velocity.Y == Approx(-1.0f));
	CHECK(velocity.Z == Approx(0.5f));
}

TEST_CASE("persistent world loads integrate once per physics step", "[physics][bodymotion]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.loads");
	engine::physics::PreparePhysicsWorld(store);
	const Entity part = DynamicPart(store, 2.0f);

	REQUIRE(engine::physics::SetAppliedForce(store, part, Vector3{4.0f, 0.0f, 0.0f}));
	REQUIRE(engine::physics::SetAppliedTorque(store, part, Vector3{0.0f, 2.0f, 0.0f}));
	CHECK(engine::physics::AppliedForce(store, part) == Vector3{4.0f, 0.0f, 0.0f});
	CHECK(engine::physics::AppliedTorque(store, part) == Vector3{0.0f, 2.0f, 0.0f});

	engine::ecs::Scheduler scheduler;
	engine::physics::RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0 / 60.0);
	CHECK(engine::physics::LinearVelocity(store, part).X == Approx(1.0f / 30.0f));
	CHECK(engine::physics::AngularVelocity(store, part).Y == Approx(0.1f));

	scheduler.Tick(store, 1.0 / 60.0);
	CHECK(engine::physics::LinearVelocity(store, part).X == Approx(1.0f / 15.0f));
	CHECK(engine::physics::AngularVelocity(store, part).Y == Approx(0.2f));

	REQUIRE(engine::physics::SetAppliedForce(store, part, Vector3::Zero));
	REQUIRE(engine::physics::SetAppliedTorque(store, part, Vector3::Zero));
	scheduler.Tick(store, 1.0 / 60.0);
	CHECK(engine::physics::LinearVelocity(store, part).X == Approx(1.0f / 15.0f));
	CHECK(engine::physics::AngularVelocity(store, part).Y == Approx(0.2f));
}

TEST_CASE("persistent world loads run for every fast-world substep", "[physics][bodymotion]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.fast-loads");
	engine::physics::PreparePhysicsWorld(store);
	engine::physics::SetPhysicsTickRate(store, 120.0);
	const Entity part = DynamicPart(store, 2.0f);

	REQUIRE(engine::physics::SetAppliedForce(store, part, Vector3{4.0f, 0.0f, 0.0f}));
	REQUIRE(engine::physics::SetAppliedTorque(store, part, Vector3{0.0f, 2.0f, 0.0f}));
	engine::ecs::Scheduler scheduler;
	engine::physics::RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0 / 60.0);

	// Two 1/120-second steps make the same one-sixtieth-second load response
	// as one ordinary step. Applying only in the Simulation phase would halve it.
	CHECK(engine::physics::LinearVelocity(store, part).X == Approx(1.0f / 30.0f));
	CHECK(engine::physics::AngularVelocity(store, part).Y == Approx(0.1f));
}

TEST_CASE("a persistent world load restores a sleeping body's motion", "[physics][bodymotion]") {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.wake-loads");
	engine::physics::PreparePhysicsWorld(store);
	const Entity part = DynamicPart(store, 2.0f);

	REQUIRE(engine::physics::SetAppliedForce(store, part, Vector3{4.0f, 0.0f, 0.0f}));
	store.Remove<engine::scene::Motion>(part);

	engine::ecs::Scheduler scheduler;
	engine::physics::RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0 / 60.0);

	CHECK(engine::physics::LinearVelocity(store, part).X == Approx(1.0f / 30.0f));
}

TEST_CASE(
	"only simulated non-static bodies accept velocity and only dynamics accept impulse",
	"[physics][bodymotion]"
) {
	engine::scene::RegisterSceneClasses();
	Store store("physics.bodymotion.kinds");
	engine::physics::PreparePhysicsWorld(store);

	engine::scene::PartDesc staticDescription;
	const Entity staticPart = engine::scene::MakePart(store, staticDescription);
	CHECK_FALSE(engine::physics::SetLinearVelocity(store, staticPart, Vector3::XAxis));

	const Entity kinematic = DynamicPart(store);
	store.GetMutable<engine::scene::RigidBody>(kinematic)->Kind = engine::scene::BodyKind::Kinematic;
	CHECK(engine::physics::SetLinearVelocity(store, kinematic, Vector3::XAxis));
	CHECK_FALSE(engine::physics::ApplyImpulse(store, kinematic, Vector3::XAxis));
	CHECK_FALSE(engine::physics::SetAppliedForce(store, kinematic, Vector3::XAxis));
	CHECK_FALSE(engine::physics::SetAppliedTorque(store, kinematic, Vector3::XAxis));

	const Entity dynamic = DynamicPart(store);
	CHECK_FALSE(
		engine::physics::SetAppliedForce(
			store, dynamic, Vector3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f}
		)
	);
}
