#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

// What a world made of these parts actually does.
//
// One suite for the behavioural cases `v02v03v04.md` §3.7 names, rather than
// one case each bolted onto the suite of whichever header happened to be
// involved. Every one of them spans the whole pipeline — integrate, index,
// pair, intersect, solve, publish — so attaching them to `NarrowPhase.hpp` or
// `Solver.hpp` would make either suite fail for a reason that is not about the
// header it is named after.
//
// The remaining §3.7 case, a raycast against a rotated box, is in
// `tests/Query.cpp`: it is about one function and does not need a tick.

TEST_SUITE_ID("engine.physics.behaviour")
// The whole pipeline, registered the way a host registers it.
TEST_DEPENDS("engine.physics.pipeline")
// Contacts, impulses and the sleeping decision.
TEST_DEPENDS("engine.physics.solver")
// The six pairs, and the two cylinder cases these scenes rest on.
TEST_DEPENDS("engine.physics.narrowphase")

using Catch::Approx;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::RegisterPhysicsSystems;
using engine::scene::BodyKind;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Transform;

namespace {
	constexpr float SIXTY_HERTZ = 1.0f / 60.0f;
	constexpr float THIRTY_HERTZ = 1.0f / 30.0f;
	constexpr float TWO_FORTY_HERTZ = 1.0f / 240.0f;
	constexpr float GRAVITY = 9.81f;
	constexpr float QUARTER_TURN = 1.5707963268f;

	// Weight, as a `PreSimulation` system.
	//
	// **Not part of this module**, and the omission is deliberate rather than
	// unfinished: `v02v03v04.md` §3.5 has no gravity row, `scene::RigidBody`
	// has no gravity scale, and a world with no down — an orbital simulation, a
	// top-down game — should not have to switch one off. A scene that wants
	// weight adds it, which is exactly what a host would do.
	//
	// `PreSimulation` rather than `Simulation`, because a system sharing a
	// phase with `physics.simulation` has no ordering against it and gravity
	// applied after the integrate is one tick of free fall lost.
	void ApplyGravity(Store &store) {
		const float delta = store.Time().Delta;
		store.Each<Motion, const RigidBody>([delta](Entity, Motion &motion, const RigidBody &body) {
			if (body.Kind == BodyKind::Dynamic) {
				motion.Linear.Y = motion.Linear.Y - GRAVITY * delta;
			}
		});
	}

	// A part, described the way the scenes below want one.
	struct Part {
		Vector3 Position{};
		Vector3 Extent{0.5f, 0.5f, 0.5f};
		ShapeKind Shape = ShapeKind::Box;
		CFrame Rotation{};
		bool Anchored = false;
	};

	Entity Place(Store &store, const Part &part) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{part.Position, part.Rotation.Rotation()}});

		Collider collider;
		collider.Shape = part.Shape;
		collider.Extent = part.Extent;
		store.Set<Collider>(entity, collider);

		if (!part.Anchored) {
			store.Set<Motion>(entity, Motion{});
			store.Set<RigidBody>(entity, RigidBody{});
		}
		return entity;
	}

	Entity Floor(Store &store) {
		return Place(
			store,
			Part{
				.Position = Vector3{0.0f, -1.0f, 0.0f},
				.Extent = Vector3{16.0f, 1.0f, 16.0f},
				.Anchored = true
			}
		);
	}

	// A scheduler with gravity in front of the physics steps.
	void Drive(Scheduler &scheduler) {
		scheduler.Add("test.gravity", Phase::PreSimulation, ApplyGravity);
		RegisterPhysicsSystems(scheduler);
	}

	float HeightOf(const Store &store, Entity entity) {
		return store.Get<Transform>(entity)->Frame.Position.Y;
	}

	Vector3 PositionOf(const Store &store, Entity entity) {
		return store.Get<Transform>(entity)->Frame.Position;
	}

	// How far a frame's up vector has tipped away from world up, in radians.
	float TiltOf(const Store &store, Entity entity) {
		const float upright = store.Get<Transform>(entity)->Frame.UpVector().Dot(Vector3::YAxis);
		const float clamped = upright > 1.0f ? 1.0f : (upright < -1.0f ? -1.0f : upright);
		return std::acos(clamped);
	}
}

TEST_CASE("a dropped box comes to rest and stays at rest", "[physics][behaviour]") {
	// **The case that catches jitter at a resting contact**, and the reason
	// box-box needs a clipped multi-point manifold: one contact point is one
	// constraint, so a box on a floor pivots about it and rocks, and the
	// rocking never damps because every tick is a fresh single constraint.
	Store store("behaviour.dropped");
	PreparePhysicsWorld(store, 4.0f);
	Floor(store);
	const Entity crate = Place(store, Part{.Position = Vector3{0.0f, 3.0f, 0.0f}});

	Scheduler scheduler;
	Drive(scheduler);

	for (int tick = 0; tick < 180; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}

	// On the floor, whose top face is at zero, so a half-metre box rests with
	// its centre at a half metre. The tolerance is the solver's steady-state
	// overlap and nothing more — a millimetre and a half.
	CHECK(HeightOf(store, crate) == Approx(0.5f).margin(0.005));
	CHECK(TiltOf(store, crate) == Approx(0.0f).margin(0.01));

	// Asleep, so it is not merely still — it has stopped costing anything.
	CHECK(store.Resource<PhysicsWorld>()->Sleeping(crate));

	// And it stays there. Ten more seconds of gravity move it by nothing at
	// all, because a sleeping row has no `Motion` for gravity to reach.
	const Vector3 settled = PositionOf(store, crate);
	for (int tick = 0; tick < 600; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}
	CHECK(PositionOf(store, crate).X == Approx(settled.X));
	CHECK(PositionOf(store, crate).Y == Approx(settled.Y));
	CHECK(PositionOf(store, crate).Z == Approx(settled.Z));
}

TEST_CASE("a cylinder rests on its flat end without wobble or drift", "[physics][behaviour]") {
	// One of the two cases the exact narrow phase was chosen for. A cap against
	// a face is a disc of contact, and a manifold that reported it as one point
	// would let the cylinder tip over the moment it landed.
	Store store("behaviour.cylinder.upright");
	PreparePhysicsWorld(store, 4.0f);
	Floor(store);
	const Entity barrel = Place(
		store,
		Part{
			.Position = Vector3{0.0f, 2.0f, 0.0f},
			.Extent = Vector3{0.5f, 1.0f, 0.0f},
			.Shape = ShapeKind::Cylinder
		}
	);

	Scheduler scheduler;
	Drive(scheduler);
	for (int tick = 0; tick < 180; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}

	CHECK(HeightOf(store, barrel) == Approx(1.0f).margin(0.01));

	// Upright, and in the same place it landed. A wobble shows up as tilt and a
	// drift as a sideways offset, and the disc contact is what prevents both.
	CHECK(TiltOf(store, barrel) == Approx(0.0f).margin(0.02));
	CHECK(PositionOf(store, barrel).X == Approx(0.0f).margin(0.01));
	CHECK(PositionOf(store, barrel).Z == Approx(0.0f).margin(0.01));
}

TEST_CASE("a cylinder rests on its side without sinking", "[physics][behaviour]") {
	// The other one. A barrel against a face touches along a line, so the
	// manifold has two points; with one it would roll in place, and with none
	// of the barrel case at all it would sink to wherever the cap test decided.
	Store store("behaviour.cylinder.side");
	PreparePhysicsWorld(store, 4.0f);
	Floor(store);
	const Entity barrel = Place(
		store,
		Part{
			.Position = Vector3{0.0f, 2.0f, 0.0f},
			.Extent = Vector3{0.5f, 1.5f, 0.0f},
			.Shape = ShapeKind::Cylinder,
			.Rotation = CFrame::Angles(0.0f, 0.0f, QUARTER_TURN),
		}
	);

	Scheduler scheduler;
	Drive(scheduler);
	for (int tick = 0; tick < 180; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}

	// Lying on its side, its centre is one radius above the floor.
	const float landed = HeightOf(store, barrel);
	CHECK(landed == Approx(0.5f).margin(0.01));

	// **Sinking is a drift and not an offset**, so the check that matters is
	// that another three seconds change nothing. A solver whose correction was
	// too weak passes the line above and fails this one.
	for (int tick = 0; tick < 180; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}
	CHECK(HeightOf(store, barrel) == Approx(landed).margin(0.0005));
}

TEST_CASE("a stack of boxes does not drift", "[physics][behaviour]") {
	// Four boxes and a floor. Each contact has to hold the weight of everything
	// above it, which is what the warm start is for — without last tick's
	// impulses to start from, sixteen sweeps do not reach the bottom of a stack
	// and it sinks a little further every tick.
	Store store("behaviour.stack");
	PreparePhysicsWorld(store, 4.0f);
	Floor(store);

	Entity boxes[4];
	for (int index = 0; index < 4; index++) {
		boxes[index] =
			Place(store, Part{.Position = Vector3{0.0f, 0.5f + static_cast<float>(index) * 1.02f, 0.0f}});
	}

	Scheduler scheduler;
	Drive(scheduler);
	for (int tick = 0; tick < 240; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}

	float settled[4];
	for (int index = 0; index < 4; index++) {
		settled[index] = HeightOf(store, boxes[index]);
		// Still in a stack: each box a metre above the one below, to within the
		// overlap the solver allows.
		CHECK(settled[index] == Approx(0.5f + static_cast<float>(index)).margin(0.02));
		CHECK(std::abs(PositionOf(store, boxes[index]).X) < 0.02f);
		CHECK(std::abs(PositionOf(store, boxes[index]).Z) < 0.02f);
	}

	for (int tick = 0; tick < 240; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
	}
	for (int index = 0; index < 4; index++) {
		CHECK(HeightOf(store, boxes[index]) == Approx(settled[index]).margin(0.0005));
	}
}

namespace {
	// A scene of several shapes falling onto a floor and onto each other, run
	// for `ticks` at `delta` and snapshotted.
	//
	// **Big enough that a parallel solver would be caught.** An earlier version
	// of this scene held four bodies, which is a dozen contact rows — under the
	// count at which `Jobs::For` bothers to dispatch, so a solver made parallel
	// ran inline, stayed serial and stayed deterministic. The case passed and
	// proved nothing. Six columns of three, plus their neighbours, is a few
	// hundred rows and well past that floor.
	//
	// Deliberately built with no entity names: `Store::Save` writes the name
	// table, and two runs meant to compare byte for byte must not differ by
	// what they were called.
	std::vector<std::byte> RunScene(const char *name, float delta, int ticks) {
		Store store(name);
		PreparePhysicsWorld(store, 4.0f);
		Floor(store);

		for (int column = 0; column < 6; column++) {
			const float x = static_cast<float>(column) * 1.4f - 3.5f;
			Place(store, Part{.Position = Vector3{x, 0.6f, 0.0f}});
			Place(store, Part{.Position = Vector3{x + 0.1f, 1.7f, 0.05f}});
			Place(
				store,
				Part{
					.Position = Vector3{x, 2.9f, 0.0f},
					.Extent = Vector3{0.5f, 0.0f, 0.0f},
					.Shape = ShapeKind::Sphere,
				}
			);
			Place(
				store,
				Part{
					.Position = Vector3{x - 0.05f, 4.2f, 0.0f},
					.Extent = Vector3{0.5f, 0.5f, 0.0f},
					.Shape = ShapeKind::Cylinder,
					.Rotation = CFrame::Angles(0.2f, 0.0f, 0.4f),
				}
			);
		}

		Scheduler scheduler;
		Drive(scheduler);
		for (int tick = 0; tick < ticks; tick++) {
			scheduler.Tick(store, delta);
		}

		ByteWriter writer;
		REQUIRE(store.Save(writer));
		const std::span<const std::byte> bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}
}

TEST_CASE("two runs of one scene agree byte for byte", "[physics][behaviour]") {
	// Same binary, same platform, same result — `v02v03v04.md` §2.4 and §3.5.
	// A snapshot rather than a field comparison, because a snapshot is what
	// `just determinism` compares and it also catches a component whose padding
	// reached the file uninitialised.
	const std::vector<std::byte> first = RunScene("behaviour.determinism", SIXTY_HERTZ, 120);
	const std::vector<std::byte> second = RunScene("behaviour.determinism", SIXTY_HERTZ, 120);

	REQUIRE_FALSE(first.empty());
	CHECK(first == second);

	// And a different number of ticks is a different world, so the comparison
	// above is comparing something rather than two copies of one buffer.
	CHECK_FALSE(RunScene("behaviour.determinism", SIXTY_HERTZ, 121) == first);
}

TEST_CASE("the same scene is reproducible at 30 and at 240 hertz", "[physics][behaviour]") {
	// **What "agrees at 30 and 240 hertz" can mean and what it cannot.** A
	// fixed-step integrator taking a different step lands somewhere different —
	// that is arithmetic, not a bug — so the property held here is the one that
	// matters for a recording: at each rate, two runs of one scene are
	// identical, and both settle into the same resting state.
	const std::vector<std::byte> slowFirst = RunScene("behaviour.rate.slow", THIRTY_HERTZ, 90);
	const std::vector<std::byte> slowSecond = RunScene("behaviour.rate.slow", THIRTY_HERTZ, 90);
	CHECK(slowFirst == slowSecond);

	const std::vector<std::byte> fastFirst = RunScene("behaviour.rate.fast", TWO_FORTY_HERTZ, 720);
	const std::vector<std::byte> fastSecond = RunScene("behaviour.rate.fast", TWO_FORTY_HERTZ, 720);
	CHECK(fastFirst == fastSecond);

	// Three seconds of falling at either rate puts the box on the floor. The
	// bytes differ; where things end up does not.
	const auto restingHeight = [](float delta, int ticks) {
		Store store("behaviour.rate.height");
		PreparePhysicsWorld(store, 4.0f);
		Floor(store);
		const Entity crate = Place(store, Part{.Position = Vector3{0.0f, 3.0f, 0.0f}});

		Scheduler scheduler;
		Drive(scheduler);
		for (int tick = 0; tick < ticks; tick++) {
			scheduler.Tick(store, delta);
		}
		return HeightOf(store, crate);
	};

	CHECK(restingHeight(THIRTY_HERTZ, 90) == Approx(0.5f).margin(0.01));
	CHECK(restingHeight(SIXTY_HERTZ, 180) == Approx(0.5f).margin(0.01));
	CHECK(restingHeight(TWO_FORTY_HERTZ, 720) == Approx(0.5f).margin(0.01));
}

TEST_CASE("two worlds ticked in parallel equal two ticked serially", "[physics][behaviour]") {
	// Worlds never collide with each other — a portal is a message, not a
	// contact — so physics across worlds is embarrassingly parallel and needs
	// no concurrency story of its own. What would break that is shared state:
	// a module-scope scratch buffer, a static, a cache keyed by anything but
	// the world. This is the case that finds one.
	//
	// The two scenes differ, so a bug that made both threads write one buffer
	// shows up as an answer that is neither.
	const auto scene = [](const char *name, int ticks) { return RunScene(name, SIXTY_HERTZ, ticks); };

	const std::vector<std::byte> serialFirst = scene("behaviour.parallel.a", 90);
	const std::vector<std::byte> serialSecond = scene("behaviour.parallel.b", 137);

	std::vector<std::byte> parallelFirst;
	std::vector<std::byte> parallelSecond;

	// Each store is constructed inside the thread that ticks it, because a
	// `Store` binds its owning thread on construction and the affinity check is
	// part of what is being relied on here.
	std::thread first([&parallelFirst, &scene] { parallelFirst = scene("behaviour.parallel.a", 90); });
	std::thread second([&parallelSecond, &scene] { parallelSecond = scene("behaviour.parallel.b", 137); });
	first.join();
	second.join();

	REQUIRE_FALSE(serialFirst.empty());
	CHECK(parallelFirst == serialFirst);
	CHECK(parallelSecond == serialSecond);
	CHECK_FALSE(serialFirst == serialSecond);
}

// --- what a part is made of ---------------------------------------------------

// **A part's own numbers beat its material's, and the solver is where that has
// to be true.** `scene::PhysicsProperties` is the crate that is deliberately
// heavier or the ramp that is deliberately slippery; a component the properties
// panel writes and nothing reads would be four floats of decoration.
TEST_CASE("a custom density is the mass the solver uses", "[physics][behaviour]") {
	using engine::scene::PhysicsProperties;

	// **Momentum, because resting on a floor proves nothing.** A box at rest
	// sits at the same height whatever it weighs, so a case that only dropped
	// one would pass with the density thrown away. What mass decides is how
	// much of a shove survives a collision: an inelastic hit leaves the pair
	// moving at `m1 v / (m1 + m2)`, so the same shove into a heavier target
	// leaves a slower pair — and the two runs below differ in nothing except
	// the target's density.
	const auto shove = [](bool dense) {
		Store store(dense ? "physics_test.density.heavy" : "physics_test.density.light");
		Scheduler scheduler;
		RegisterPhysicsSystems(scheduler);
		PreparePhysicsWorld(store);

		// No gravity and no floor: this is about one impact, and a floor would
		// add friction to what is being measured.
		const Entity pusher = Place(store, Part{.Position = Vector3{-2.0f, 0.0f, 0.0f}});
		const Entity target = Place(store, Part{.Position = Vector3{0.0f, 0.0f, 0.0f}});

		if (dense) {
			PhysicsProperties heavy;
			heavy.Custom = true;
			heavy.Density = 8.0f;
			store.Set<PhysicsProperties>(target, heavy);
		}

		Motion moving;
		moving.Linear = Vector3{4.0f, 0.0f, 0.0f};
		store.Set<Motion>(pusher, moving);

		for (int tick = 0; tick < 60; tick++) {
			scheduler.Tick(store, SIXTY_HERTZ);
		}
		return store.Get<Motion>(target)->Linear.X;
	};

	const float light = shove(false);
	const float heavy = shove(true);

	// The lighter target is carried away faster than the heavier one. Both are
	// moving — a target that never moved would mean the impact never happened
	// and the comparison would be between two zeros.
	INFO("light " << light << " heavy " << heavy);
	CHECK(light > 0.1f);
	CHECK(heavy > 0.0f);
	CHECK(heavy < light * 0.6f);

	// And the mass a properties panel would show is the one the density
	// implies — one cubic metre at eight kilograms a metre.
	// The mass rule on its own, with no classes registered: `Place` builds the
	// three components by hand, which is all `MassOf` reads.
	Store store("physics_test.density.mass");
	const Entity part = Place(store, Part{});

	PhysicsProperties eight;
	eight.Custom = true;
	eight.Density = 8.0f;
	store.Set<PhysicsProperties>(part, eight);

	CHECK(
		engine::scene::MassOf(
			*store.Get<Collider>(part), *store.Get<RigidBody>(part), store.Get<PhysicsProperties>(part)
		) == Approx(8.0f)
	);
}

TEST_CASE("a custom elasticity makes a part bounce", "[physics][behaviour]") {
	using engine::scene::PhysicsProperties;

	Store store("physics_test.elastic");
	Scheduler scheduler;
	Drive(scheduler);
	PreparePhysicsWorld(store);

	Floor(store);

	const Entity dead = Place(store, Part{.Position = Vector3{-2.0f, 4.0f, 0.0f}});
	const Entity bouncy = Place(store, Part{.Position = Vector3{2.0f, 4.0f, 0.0f}});

	PhysicsProperties rubber;
	rubber.Custom = true;
	rubber.Elasticity = 0.8f;
	store.Set<PhysicsProperties>(bouncy, rubber);

	// Long enough to hit, and short enough that the bounce has not been
	// re-absorbed. The comparison is between the two rather than against a
	// number, because what is under test is that the override reached the
	// solver — not what a particular restitution integrates to.
	float highest = 0.0f;
	bool landed = false;
	for (int tick = 0; tick < 120; tick++) {
		scheduler.Tick(store, SIXTY_HERTZ);
		landed = landed || HeightOf(store, bouncy) < 0.6f;
		if (landed) {
			highest = std::max(highest, HeightOf(store, bouncy));
		}
	}

	CHECK(landed);
	CHECK(highest > 0.7f);

	// The one with no override took the material's restitution, which is zero:
	// it stops dead.
	CHECK(HeightOf(store, dead) == Approx(0.5f).margin(0.05f));
}
