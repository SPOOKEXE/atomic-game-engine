#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_SUITE_ID("engine.physics.clock")
// The clock is a resource under an explicit name, registered by the same call
// that registers `PhysicsWorld`, and a snapshot is what proves both.
TEST_DEPENDS("engine.physics.pipeline")
// What the clock's delta is fed to.
TEST_DEPENDS("engine.physics.integrate")
// `Store::Time().Delta` is what a tick charges to it, and the scheduler is what
// advances that.
TEST_DEPENDS("engine.ecs.scheduler")

using Catch::Approx;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::physics::BeginPhysicsStep;
using engine::physics::BeginPhysicsTick;
using engine::physics::FirstPhysicsStepOfTick;
using engine::physics::PhysicsClock;
using engine::physics::PhysicsClockOf;
using engine::physics::PhysicsStepSeconds;
using engine::physics::PhysicsTickRate;
using engine::physics::PreparePhysicsWorld;
using engine::physics::RegisterPhysicsSystems;
using engine::physics::SetPhysicsTickRate;
using engine::scene::Motion;
using engine::scene::Transform;

namespace {
	constexpr float TICK = 1.0f / 60.0f;

	// One body moving at a metre per second along X, and nothing to hit. The
	// distance it has covered is a direct reading of how much simulated time
	// the steps integrated, which is what every case here is really asking.
	Entity Drift(Store &store) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{Vector3::Zero}});
		store.Set<Motion>(entity, Motion{Vector3{1.0f, 0.0f, 0.0f}, Vector3::Zero});
		return entity;
	}

	float TravelledX(const Store &store, Entity entity) {
		return store.Get<Transform>(entity)->Frame.Position.X;
	}

	// How many steps one tick of `delta` produces at `rate`, driving the clock
	// directly rather than through a scheduler.
	int StepsInOneTick(Store &store, float delta) {
		store.AdvanceTick(delta);
		BeginPhysicsTick(store);

		int steps = 0;
		while (BeginPhysicsStep(store)) {
			steps++;
		}
		return steps;
	}
}

TEST_CASE("a prepared world follows its tick rate", "[physics][clock]") {
	Store store("clock.default");
	PreparePhysicsWorld(store);

	// Zero is "follow the world", which is what every world in this repository
	// is and what physics did before this clock existed.
	CHECK(PhysicsTickRate(store) == 0.0);

	CHECK(StepsInOneTick(store, TICK) == 1);

	const PhysicsClock *clock = PhysicsClockOf(store);
	REQUIRE(clock != nullptr);
	CHECK(clock->Steps == 1);
	CHECK(clock->DroppedSteps == 0);

	// The step is the tick, to the bit rather than to a tolerance: a world that
	// said nothing about physics must integrate exactly what it always did.
	CHECK(clock->Delta == TICK);
}

TEST_CASE("a world with no clock reads the tick as its step", "[physics][clock]") {
	// **The fallback every suite and benchmark in this module depends on.**
	// They call `IntegrateMotion` directly against a store nobody prepared, and
	// the delta they mean is the world's tick. Without this each of them would
	// integrate zero seconds and assert against a body that never moved.
	Store store("clock.unprepared");
	store.AdvanceTick(TICK);

	CHECK(PhysicsClockOf(store) == nullptr);
	CHECK(PhysicsStepSeconds(store) == TICK);
}

TEST_CASE("a slower rate steps on some ticks and not others", "[physics][clock]") {
	Store store("clock.slower");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 30.0);

	CHECK(PhysicsTickRate(store) == Approx(30.0));

	// Half the world's rate, so one tick in two carries a step - and the step
	// is a thirtieth rather than the tick it was charged from.
	CHECK(StepsInOneTick(store, TICK) == 0);
	CHECK(StepsInOneTick(store, TICK) == 1);
	CHECK(PhysicsClockOf(store)->Delta == Approx(1.0f / 30.0f));

	CHECK(StepsInOneTick(store, TICK) == 0);
	CHECK(StepsInOneTick(store, TICK) == 1);

	CHECK(PhysicsClockOf(store)->Steps == 2);
}

TEST_CASE("a faster rate steps more than once in a tick", "[physics][clock]") {
	Store store("clock.faster");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 120.0);

	CHECK(StepsInOneTick(store, TICK) == 2);
	CHECK(PhysicsClockOf(store)->Delta == Approx(1.0f / 120.0f));
	CHECK(StepsInOneTick(store, TICK) == 2);
	CHECK(PhysicsClockOf(store)->DroppedSteps == 0);
}

TEST_CASE("only the first step of a tick is the first step of a tick", "[physics][clock]") {
	// **What `NarrowPhase` reads to decide whether to clear the contact
	// events.** They belong to the tick rather than to the step, so a touch
	// that began on a world's second physics step of one is still a touch that
	// happened this tick.
	Store store("clock.firststep");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 180.0);

	// A world with no step running yet answers `true`, which is what keeps the
	// clear unconditional for everything that is not multi-stepping.
	CHECK(FirstPhysicsStepOfTick(store));

	store.AdvanceTick(TICK);
	BeginPhysicsTick(store);

	REQUIRE(BeginPhysicsStep(store));
	CHECK(FirstPhysicsStepOfTick(store));

	REQUIRE(BeginPhysicsStep(store));
	CHECK_FALSE(FirstPhysicsStepOfTick(store));

	REQUIRE(BeginPhysicsStep(store));
	CHECK_FALSE(FirstPhysicsStepOfTick(store));

	// And the next tick starts the count again rather than carrying it.
	store.AdvanceTick(TICK);
	BeginPhysicsTick(store);
	REQUIRE(BeginPhysicsStep(store));
	CHECK(FirstPhysicsStepOfTick(store));
}

TEST_CASE("a rate that does not divide the tick keeps a fixed step", "[physics][clock]") {
	// **The property that matters is the step length, not the pattern.** 25 Hz
	// inside a 60 Hz world produces steps on an uneven run of ticks, and every
	// one of them is a twenty-fifth: the remainder is carried rather than
	// folded into the next step, so a recording replays whatever the tick
	// pattern happened to be.
	Store store("clock.uneven");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 25.0);

	int steps = 0;
	for (int tick = 0; tick < 60; tick++) {
		const int owed = StepsInOneTick(store, TICK);
		steps += owed;
		if (owed > 0) {
			CHECK(PhysicsClockOf(store)->Delta == Approx(1.0f / 25.0f));
		}
	}

	// A second of ticks buys a second of steps. Not exactly 25: the last
	// interval may be part-paid and waiting in the accumulator, which is the
	// whole point of carrying a remainder.
	CHECK(steps >= 24);
	CHECK(steps <= 25);
}

TEST_CASE("a stall is given up on rather than caught up", "[physics][clock]") {
	Store store("clock.stall");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 1000.0);

	// One 60 Hz tick owes about sixteen steps at 1000 Hz, which is twice the
	// cap. Carrying the excess is the death spiral `FixedTimestep` refuses, so
	// the count is clamped and the difference is reported rather than hidden.
	CHECK(StepsInOneTick(store, TICK) == PhysicsClock::MAXIMUM_STEPS_PER_TICK);

	const PhysicsClock *clock = PhysicsClockOf(store);
	CHECK(clock->DroppedSteps > 0);

	// And the accumulator was emptied with it, so the next tick starts level
	// instead of arriving already over the cap.
	CHECK(clock->Accumulator == Approx(0.0));
}

TEST_CASE("changing the rate keeps the time already charged", "[physics][clock]") {
	Store store("clock.change");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 30.0);

	// One tick in, so a thirtieth is half paid for.
	CHECK(StepsInOneTick(store, TICK) == 0);
	CHECK(PhysicsClockOf(store)->Accumulator == Approx(TICK));

	// Raising the rate must not throw that away - a world that skipped forward
	// whenever an author touched a slider is worse than one that does not
	// change at all.
	SetPhysicsTickRate(store, 60.0);
	CHECK(PhysicsClockOf(store)->Accumulator == Approx(TICK));
	CHECK(StepsInOneTick(store, TICK) == 2);
}

TEST_CASE("a negative rate reads as following the tick", "[physics][clock]") {
	Store store("clock.negative");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, -5.0);

	CHECK(PhysicsTickRate(store) == 0.0);
	CHECK(StepsInOneTick(store, TICK) == 1);
	CHECK(PhysicsClockOf(store)->Delta == TICK);
}

TEST_CASE("a rate that is not a number lands on zero", "[physics][clock]") {
	// **A rate arrives from a game file and from a snapshot, and §7 calls both
	// hostile.** `1.0 / NaN` divided into the accumulator and cast to an
	// `int32_t` is undefined behaviour rather than an odd step count, so the
	// guard is a positive test and not `<= 0`.
	Store store("clock.nan");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, std::numeric_limits<double>::quiet_NaN());

	CHECK(PhysicsTickRate(store) == 0.0);
	CHECK(StepsInOneTick(store, TICK) == 1);
}

TEST_CASE("an unbounded rate is held at the ceiling", "[physics][clock]") {
	// The other half of the same hazard: infinity survives `> 0`, and the step
	// count derived from it does not fit an `int32_t` at all.
	Store store("clock.infinite");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, std::numeric_limits<double>::infinity());

	CHECK(PhysicsTickRate(store) == Approx(PhysicsClock::MAXIMUM_RATE));

	// And what it actually runs is the per-tick cap, with the rest reported
	// rather than swallowed.
	CHECK(StepsInOneTick(store, TICK) == PhysicsClock::MAXIMUM_STEPS_PER_TICK);
	CHECK(PhysicsClockOf(store)->DroppedSteps > 0);
}

TEST_CASE("the pipeline steps a slow world half as often", "[physics][clock]") {
	// The same reading through the registered systems, which is where the two
	// phases have to agree about whether a step ran at all.
	Store store("clock.pipeline");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 30.0);

	const Entity body = Drift(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	for (int tick = 0; tick < 60; tick++) {
		scheduler.Tick(store, TICK);
	}

	// A second of drifting at a metre per second is a metre, whichever rate it
	// was integrated at. Halving the rate halves the number of steps and
	// doubles their length; it does not halve the distance.
	CHECK(TravelledX(store, body) == Approx(1.0f).margin(0.05f));
	CHECK(PhysicsClockOf(store)->Steps == 30);
}

TEST_CASE("the pipeline steps a fast world twice a tick", "[physics][clock]") {
	Store store("clock.pipeline.fast");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 120.0);

	const Entity body = Drift(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	for (int tick = 0; tick < 60; tick++) {
		scheduler.Tick(store, TICK);
	}

	CHECK(TravelledX(store, body) == Approx(1.0f).margin(0.05f));
	CHECK(PhysicsClockOf(store)->Steps == 120);
}

TEST_CASE("the rate survives a snapshot and the counters do not", "[physics][clock]") {
	Store store("clock.snapshot");
	PreparePhysicsWorld(store);
	SetPhysicsTickRate(store, 90.0);

	StepsInOneTick(store, TICK);
	REQUIRE(PhysicsClockOf(store)->Steps > 0);

	ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored("clock.snapshot");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	// The rate is what an author chose, so it crosses. Everything else is a
	// function of the rate and of the ticks that have run since, and a restored
	// world has run none - so it owes no steps for the time the file spent on
	// disk.
	CHECK(PhysicsTickRate(restored) == Approx(90.0));

	const PhysicsClock *clock = PhysicsClockOf(restored);
	REQUIRE(clock != nullptr);
	CHECK(clock->Steps == 0);
	CHECK(clock->Owed == 0);
	CHECK(clock->Accumulator == Approx(0.0));
	CHECK_FALSE(clock->Stepping);
}

TEST_CASE("a world with the systems and no clock still integrates", "[physics][clock]") {
	// **The regression the clock could have introduced.** Registering the
	// systems without preparing the world is a misconfiguration, and every
	// other step in the pipeline already refuses it loudly through
	// `PreparedWorld`. Gating the integration on a clock that world does not
	// have would have turned the loud failure into a silent one: nothing moves,
	// nothing says why.
	engine::physics::RegisterPhysicsComponents();

	Store store("clock.unprepared.pipeline");
	const Entity body = Drift(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	for (int tick = 0; tick < 60; tick++) {
		scheduler.Tick(store, TICK);
	}

	CHECK(TravelledX(store, body) == Approx(1.0f).margin(0.05f));
}

TEST_CASE("the clock is registered under an explicit name", "[physics][clock]") {
	engine::physics::RegisterPhysicsComponents();
	CHECK(engine::ecs::Components::Find(Name("physics.PhysicsClock")).IsValid());
}
