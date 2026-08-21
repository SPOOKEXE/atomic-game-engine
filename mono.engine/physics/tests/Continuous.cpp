#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_SUITE_ID("engine.physics.continuous")
// The sweep it is built out of.
TEST_DEPENDS("engine.physics.convexquery")
// The index it asks for candidates, and the step that builds it.
TEST_DEPENDS("engine.physics.broadphase")
// The motion it reconstructs.
TEST_DEPENDS("engine.physics.integrate")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::IntegrateMotion;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::SweepFastBodies;
using engine::physics::SyncBroadphase;
using engine::scene::BodyKind;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::Simulated;
using engine::scene::Transform;

namespace {
	// Sixty hertz, the rate the rest of this module is tuned against.
	constexpr float TICK = 1.0f / 60.0f;

	// A world holding one thin anchored wall at x = 4, standing across the x
	// axis.
	//
	// **Ten centimetres thick**, which is the case the feature is for: a body
	// crossing it in one tick is on the far side before anything looks, and no
	// amount of solver iteration finds a contact that never existed at a moment
	// the pipeline sampled.
	std::unique_ptr<Store> WorldWithWall(float thickness = 0.05f) {
		auto store = std::make_unique<Store>("physics.continuous");
		PreparePhysicsWorld(*store, 4.0f);

		const Entity wall = store->Create();
		// No `Simulated` and no `Motion`. A wall is static by carrying nothing,
		// which is the whole of the v0.18 polarity.
		store->Set<Transform>(wall, Transform{CFrame{Vector3{4.0f, 0.0f, 0.0f}}});

		Collider pane;
		pane.Extent = Vector3{thickness, 5.0f, 5.0f};
		store->Set<Collider>(wall, pane);

		return store;
	}

	// A unit cube at the origin travelling along +x at `speed` metres a second.
	Entity Bullet(Store &store, float speed, float half = 0.5f) {
		const Entity body = store.Create();
		store.Set<Transform>(body, Transform{CFrame{Vector3{0.0f, 0.0f, 0.0f}}});

		Motion motion;
		motion.Linear = Vector3{speed, 0.0f, 0.0f};
		store.Set<Motion>(body, motion);

		// **The tag as well as the `Motion`.** `SweepFastBodies` asks whether the
		// world may move this, and a row with a velocity and no tag is the state
		// nothing owns - it would be skipped, and every case in this file would
		// pass by measuring a body that was never swept.
		store.Set<Simulated>(body, Simulated{});

		RigidBody rigid;
		rigid.Kind = BodyKind::Dynamic;
		rigid.Mass = 1.0f;
		store.Set<RigidBody>(body, rigid);

		Collider shape;
		shape.Extent = Vector3{half, half, half};
		store.Set<Collider>(body, shape);

		return body;
	}

	// One tick of the two steps that matter here, in the order the pipeline runs
	// them. The static index has to exist before the sweep can ask it anything,
	// which is why the sync comes first on the setup tick.
	void Tick(Store &store) {
		IntegrateMotion(store);
		SweepFastBodies(store);
		SyncBroadphase(store);
	}
}

TEST_CASE("a body that would cross a thin wall is stopped at it", "[continuous]") {
	// **The bug this exists for, end to end.** At 300 metres a second the cube
	// travels five metres in one tick and the wall is a tenth of a metre thick,
	// so the integrator alone puts it out the far side with nothing to solve.
	std::unique_ptr<Store> owned = WorldWithWall();
	Store &store = *owned;

	// One sync first, so the static index the sweep reads exists. A world's very
	// first tick has none and sweeps nothing, which `Continuous.hpp` states.
	SyncBroadphase(store);

	const Entity bullet = Bullet(store, 300.0f);
	Tick(store);

	const Transform *placed = store.Get<Transform>(bullet);
	REQUIRE(placed != nullptr);

	// Stopped at the wall's near face, which is at 4 - 0.05 = 3.95, so the
	// cube's centre is just past 3.45 - rather than out the far side at 5.0
	// where the integrator alone would have left it.
	CHECK(placed->Frame.Position.X < 3.47f);
	CHECK(placed->Frame.Position.X > 3.44f);
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 1);
}

TEST_CASE("a stopped body is left where the narrow phase can find it", "[continuous]") {
	// **The half that makes the clamp useful rather than merely safe**, and the
	// case that caught the first version. Stopping the body is only worth
	// anything if the ordinary pipeline then reports a contact: this step never
	// writes a velocity, so if the narrow phase finds nothing, the solver never
	// runs, the body keeps its three hundred metres a second, and next tick it
	// is clamped to the same place again - a bullet hanging against a wall
	// forever with nothing in the pipeline wrong. See `CONTINUOUS_BITE`.
	std::unique_ptr<Store> owned = WorldWithWall();
	Store &store = *owned;
	SyncBroadphase(store);

	Bullet(store, 300.0f);
	Tick(store);

	// The rest of the same tick. The body has been placed a millimetre into the
	// wall, so the pair is a candidate and the manifold is real.
	BroadPhase(store);
	NarrowPhase(store);
	CHECK(store.Resource<PhysicsWorld>()->Manifolds().size() >= 1);
}

TEST_CASE("a body moving slowly is not swept", "[continuous]") {
	// The threshold doing its job. A scene of settling crates must not pay a
	// distance query per candidate per body per tick for a tunnelling risk it
	// does not have - a body under gravity moves about two millimetres on the
	// tick it is dropped.
	std::unique_ptr<Store> owned = WorldWithWall();
	Store &store = *owned;
	SyncBroadphase(store);

	const Entity crate = Bullet(store, 2.0f);
	Tick(store);

	// Two metres a second is 33 mm a tick against a half-extent of 500 mm, so it
	// is nowhere near the threshold and the body is exactly where the integrator
	// left it.
	CHECK(store.Get<Transform>(crate)->Frame.Position.X == Approx(2.0f * TICK).margin(1e-5f));
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 0);
}

TEST_CASE("a body with nothing in its way is left alone", "[continuous]") {
	// The other direction the failure could run in: a step that clamped a fast
	// body whatever was ahead of it would stop every projectile in mid-air.
	std::unique_ptr<Store> owned = WorldWithWall();
	Store &store = *owned;
	SyncBroadphase(store);

	// Well above the wall, which is ten metres tall and centred on the origin.
	const Entity missed = store.Create();
	store.Set<Transform>(missed, Transform{CFrame{Vector3{0.0f, 40.0f, 0.0f}}});
	Motion motion;
	motion.Linear = Vector3{300.0f, 0.0f, 0.0f};
	store.Set<Motion>(missed, motion);
	Collider shape;
	shape.Extent = Vector3{0.5f, 0.5f, 0.5f};
	store.Set<Collider>(missed, shape);

	Tick(store);

	CHECK(store.Get<Transform>(missed)->Frame.Position.X == Approx(300.0f * TICK).margin(1e-3f));
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 0);
}

TEST_CASE("an anchored body is never swept", "[continuous]") {
	// Whatever moves an anchored part is not the integrator, so the motion this
	// step reconstructs from `Linear * delta` did not happen - and clamping it
	// against that fiction would drag authored geometry around.
	std::unique_ptr<Store> owned = WorldWithWall();
	Store &store = *owned;
	SyncBroadphase(store);

	// Keeps its `Motion` and loses the tag, which is exactly the state this case
	// is about: something with a velocity that the world may not move.
	const Entity platform = Bullet(store, 300.0f);
	store.Remove<Simulated>(platform);

	// Placed by hand, as a script or an animation would.
	store.Set<Transform>(platform, Transform{CFrame{Vector3{3.9f, 0.0f, 0.0f}}});
	SweepFastBodies(store);

	CHECK(store.Get<Transform>(platform)->Frame.Position.X == Approx(3.9f));
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 0);
}

TEST_CASE("a trigger does not stop anything", "[continuous]") {
	// A trigger reports and never pushes, so a body stopped at one would be a
	// wall made of something that is not there - and the thing an author would
	// see is a projectile hanging in the middle of a doorway.
	auto owned = std::make_unique<Store>("physics.continuous.trigger");
	Store &store = *owned;
	PreparePhysicsWorld(store, 4.0f);

	const Entity gate = store.Create();
	store.Set<Transform>(gate, Transform{CFrame{Vector3{4.0f, 0.0f, 0.0f}}});
	Collider pane;
	pane.Extent = Vector3{0.05f, 5.0f, 5.0f};
	pane.Trigger = true;
	store.Set<Collider>(gate, pane);

	SyncBroadphase(store);

	const Entity bullet = Bullet(store, 300.0f);
	Tick(store);

	CHECK(store.Get<Transform>(bullet)->Frame.Position.X == Approx(300.0f * TICK).margin(1e-3f));
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 0);
}

TEST_CASE("a world with no static geometry sweeps nothing", "[continuous]") {
	// The first tick of every world, and the whole of a world made only of
	// moving things. Stated rather than worked around - see `Continuous.hpp`.
	auto owned = std::make_unique<Store>("physics.continuous.empty");
	Store &store = *owned;
	PreparePhysicsWorld(store, 4.0f);

	const Entity bullet = Bullet(store, 300.0f);
	Tick(store);

	CHECK(store.Get<Transform>(bullet)->Frame.Position.X == Approx(300.0f * TICK).margin(1e-3f));
	CHECK(store.Resource<PhysicsWorld>()->SweptBodies() == 0);
}

TEST_CASE("a body is clamped at the wall at every speed that reaches it", "[continuous]") {
	// **The direction the failure has to run in, checked across the range.** A
	// clamp that landed well past the surface is the bug this step exists to
	// prevent, reintroduced by the fix; one that landed short of it hangs the
	// body in mid-air, which is what the first version of this did. Swept over
	// speeds because the bite is expressed as a fraction of the body's own
	// travel, and a version that expressed it as a fraction of *the motion it
	// had left* is wrong at every speed but one.
	//
	// The cube's front face starts at 0.5 and the wall's near face is at 3.95,
	// so it takes 3.45 metres of travel to arrive - 207 metres a second at this
	// tick rate. Below that the body simply has not got there, which the case
	// above covers.
	for (int step = 0; step <= 12; step++) {
		std::unique_ptr<Store> owned = WorldWithWall();
		Store &store = *owned;
		SyncBroadphase(store);

		const auto speed = 220.0f + static_cast<float>(step) * 60.0f;
		const Entity bullet = Bullet(store, speed);
		Tick(store);

		INFO("speed " << speed);
		const float front = store.Get<Transform>(bullet)->Frame.Position.X + 0.5f;

		// Past the near face by the bite and no further - not out the far side,
		// and not hanging in front of it.
		CHECK(front > 3.95f);
		CHECK(front < 3.95f + 0.01f);
	}
}
