// Scheduled work admission, sticky refusal, and complete checkpoint recovery.

#include "PipelineInternals.hpp"
#include "WorkBudgetInternal.hpp"
#include "fixtures/CellSizeParity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/Observation.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/physics/WorkBudget.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.physics.work-budget")
TEST_DEPENDS("engine.physics.pipeline")
TEST_DEPENDS("engine.physics.clock")
TEST_DEPENDS("engine.ecs.snapshot")

using namespace engine;
using namespace engine::physics;

namespace {
	constexpr float TICK = 1.0f / 60.0f;

	ecs::Entity Drift(ecs::Store &store, core::Vector3 position = {}) {
		const ecs::Entity body = store.Create();
		store.Set(body, scene::Transform{core::CFrame{position}});
		store.Set(body, scene::Motion{core::Vector3{1, 0, 0}, core::Vector3::Zero});
		return body;
	}

	void ContactScene(ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const ecs::Entity moving = Drift(store, {0, 0.9f, 0});
		store.Set(moving, scene::Collider{});
		store.Set(moving, scene::RigidBody{});
		store.Set(moving, scene::Simulated{});
		const ecs::Entity ground = store.Create();
		store.Set(ground, scene::Transform{core::CFrame{core::Vector3{0, 0, 0}}});
		store.Set(ground, scene::Collider{});
	}

	std::vector<std::byte> Save(const ecs::Store &store) {
		core::ByteWriter writer;
		REQUIRE(store.Save(writer));
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	void Load(ecs::Store &store, const std::vector<std::byte> &bytes) {
		core::ByteReader reader(bytes);
		REQUIRE(store.Load(reader));
	}

	struct WorkerPool {
		const bool OwnsPool = parallel::Jobs::WorkerCount() == 0;
		WorkerPool() {
			if (OwnsPool) parallel::Jobs::Start(1);
		}
		~WorkerPool() {
			if (OwnsPool) parallel::Jobs::Stop();
		}
	};

	struct ComputeContext {
		const bool WasSerial = parallel::ForceSerialCompute();
		~ComputeContext() {
			parallel::SetForceSerialCompute(WasSerial);
		}
	};
}

TEST_CASE("work charges accept the exact limit and retain one sticky refusal", "[physics][work-budget]") {
	ecs::Store store("work.exact");
	CHECK(PhysicsWorkBudgetOf(store) == nullptr);
	PreparePhysicsWorld(store);
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 7;
	SetPhysicsWorkLimits(store, limits);
	store.AdvanceTick(TICK);
	BeginPhysicsWorkTick(store);
	REQUIRE(PhysicsWorkActive(store));
	const uint64_t admitted = PhysicsWorkBudgetOf(store)->Used;
	REQUIRE(admitted <= limits.WorkPerTick);
	ChargePhysicsWork(store, PhysicsWorkStage::Integration, limits.WorkPerTick - admitted);
	CHECK(PhysicsWorkBudgetOf(store)->Used == limits.WorkPerTick);
	BeginPhysicsWorkTick(store);
	CHECK(PhysicsWorkBudgetOf(store)->Used == limits.WorkPerTick);
	REQUIRE_THROWS_AS(ChargePhysicsWork(store, PhysicsWorkStage::Solver, 1), std::runtime_error);
	CHECK(PhysicsWorkBudgetOf(store)->Used == limits.WorkPerTick);
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(PhysicsWorkBudgetOf(store)->ExhaustedStage == PhysicsWorkStage::Solver);

	SetPhysicsWorkLimits(store, PhysicsWorkLimits{});
	SetPhysicsPaused(store, true);
	SetPhysicsPaused(store, false);
	PreparePhysicsWorld(store);
	store.AdvanceTick(TICK);
	REQUIRE_THROWS_AS(BeginPhysicsWorkTick(store), std::runtime_error);
	REQUIRE_THROWS_AS(IntegrateMotion(store), std::runtime_error);
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(PhysicsWorkBudgetOf(store)->ExhaustedStage == PhysicsWorkStage::Solver);
}

TEST_CASE("row admission refuses before any body integrates", "[physics][work-budget]") {
	ecs::Store store("work.admission");
	PreparePhysicsWorld(store);
	const ecs::Entity body = Drift(store);
	PhysicsWorkLimits limits;
	SECTION("moving rows") {
		limits.MovingBodies = 0;
	}
	SECTION("collider rows") {
		store.Set(body, scene::Collider{});
		limits.Colliders = 0;
	}
	SECTION("editable geometry") {
		// Only a mesh some collider names is resident collision geometry.
		scene::EditableMesh mesh;
		mesh.Positions.resize(2);
		const ecs::Entity owner = store.Create();
		store.Set(owner, mesh);
		scene::Collider collider;
		collider.Shape = scene::ShapeKind::Mesh;
		collider.Geometry = scene::EditableMeshContentName(store, owner);
		store.Set(body, collider);
		limits.GeometryElements = 1;
	}
	SetPhysicsWorkLimits(store, limits);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	REQUIRE_THROWS_AS(scheduler.Tick(store, TICK), std::runtime_error);
	CHECK(store.Get<scene::Transform>(body)->Frame.Position.X == 0.0f);
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(PhysicsWorkBudgetOf(store)->ExhaustedStage == PhysicsWorkStage::Admission);
	CHECK(CopyPhysicsObservationRecords(store).empty());
}

// A region with more moving bodies than the cap simulates a rotating window
// instead of faulting: each tick moves exactly the cap there, every body gets
// a turn, a humanoid body is never parked, and the turn survives a checkpoint.
TEST_CASE("a crowded region rotates its moving window through every body", "[physics][work-budget]") {
	ecs::Store store("work.window");
	PreparePhysicsWorld(store);
	std::vector<ecs::Entity> bodies;
	for (int index = 0; index < 5; index++)
		bodies.push_back(Drift(store));
	const ecs::Entity character = Drift(store, {1000.0f, 0.0f, 0.0f});
	store.Set(character, scene::Humanoid{});
	PhysicsWorkLimits limits;
	limits.MovingBodies = 3;
	SetPhysicsWorkLimits(store, limits);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	const auto turns = [&](ecs::Entity body, float start) {
		return (store.Get<scene::Transform>(body)->Frame.Position.X - start) / TICK;
	};
	for (int tick = 1; tick <= 5; tick++) {
		REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
		CHECK(turns(character, 1000.0f) == Catch::Approx(tick).margin(0.01));
		float moved = 0.0f;
		for (const ecs::Entity body : bodies)
			moved += turns(body, 0.0f);
		CHECK(moved == Catch::Approx(3.0f * tick));
	}
	// Fifteen turns over five bodies, three consecutive a tick: each body three.
	for (const ecs::Entity body : bodies)
		CHECK(turns(body, 0.0f) == Catch::Approx(3.0f));

	const uint64_t turn = PhysicsWorkBudgetOf(store)->WindowTurn;
	CHECK(turn == 5);
	ecs::Store restored("work.window");
	Load(restored, Save(store));
	CHECK(PhysicsWorkBudgetOf(restored)->WindowTurn == turn);
}

TEST_CASE(
	"resident editable geometry is admitted without spending per-element work", "[physics][work-budget]"
) {
	// A streamed terrain keeps millions of resident elements. They are bounded
	// by GeometryElements; admitting them reads sizes and is not per-tick work.
	ecs::Store store("work.resident-geometry");
	PreparePhysicsWorld(store);
	const ecs::Entity body = Drift(store);
	scene::EditableMesh mesh;
	mesh.Positions.resize(4096);
	mesh.Indices.resize(4096);
	store.Set(store.Create(), mesh);
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 4096;
	SetPhysicsWorkLimits(store, limits);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	CHECK_FALSE(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(store.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(TICK));
}

TEST_CASE("editable mesh bakes over a tick's share wait for later ticks", "[physics][work-budget]") {
	// Two static meshes whose bakes fit one per tick: the second waits a tick
	// instead of refusing the world, and both end up resident.
	ecs::Store store("work.deferred-bake");
	PreparePhysicsWorld(store);
	std::vector<core::Name> names;
	for (int which = 0; which < 2; ++which) {
		scene::EditableMesh mesh;
		for (uint32_t triangle = 0; triangle < 1000; ++triangle) {
			const float x = static_cast<float>(triangle) + which * 2000.0f;
			for (const core::Vector3 corner :
				 {core::Vector3{x, 0, 0}, core::Vector3{x + 1, 0, 0}, core::Vector3{x, 0, 1}}) {
				mesh.Indices.push_back(static_cast<uint32_t>(mesh.Positions.size()));
				mesh.Positions.push_back(corner);
			}
		}
		const ecs::Entity owner = store.Create();
		store.Set(owner, mesh);
		store.Set(owner, scene::Transform{});
		scene::Collider collider;
		collider.Shape = scene::ShapeKind::Mesh;
		collider.Geometry = scene::EditableMeshContentName(store, owner);
		store.Set(owner, collider);
		names.push_back(collider.Geometry);
	}
	// One bake is 3,000 points and 1,000 triangles times 18: 21,000 units;
	// bakes may use half the tick's work.
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 60'000;
	SetPhysicsWorkLimits(store, limits);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	// Meshes baked whole. One still waiting holds an empty shape, so it
	// collides with nothing rather than as its bound.
	const auto resident = [&] {
		const scene::CollisionShapes *shapes = scene::CollisionShapesOf(store);
		size_t count = 0;
		for (const core::Name name : names) {
			const auto *shape = shapes != nullptr ? shapes->FindMesh(name) : nullptr;
			REQUIRE(shape != nullptr);
			count += shape->TriangleCount() == 1000 ? 1 : 0;
		}
		return count;
	};
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	CHECK(resident() == 1);
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	CHECK(resident() == 2);
	CHECK_FALSE(PhysicsWorkBudgetOf(store)->Blocked);
}

TEST_CASE("a large static mesh collider collides through cells cut at run time", "[physics][work-budget]") {
	// 20,000 triangles in two 512-unit cells. The owner's own mesh bakes empty;
	// the cells bake, follow the owner, and go when the owner stops splitting.
	ecs::Store store("work.split-collision");
	PreparePhysicsWorld(store);
	scene::EditableMesh mesh;
	mesh.Revision = 1;
	for (uint32_t triangle = 0; triangle < 20'000; ++triangle) {
		const float x = triangle < 10'000 ? 10.0f : 600.0f;
		const float z = static_cast<float>(triangle % 400);
		for (const core::Vector3 corner :
			 {core::Vector3{x, 0, z}, core::Vector3{x + 1, 0, z}, core::Vector3{x, 0, z + 1}}) {
			mesh.Indices.push_back(static_cast<uint32_t>(mesh.Positions.size()));
			mesh.Positions.push_back(corner);
		}
	}
	const ecs::Entity source = store.Create();
	store.Set(source, mesh);
	const ecs::Entity owner = store.Create();
	store.Set(owner, scene::Transform{});
	scene::Collider collider;
	collider.Shape = scene::ShapeKind::Mesh;
	collider.Geometry = scene::EditableMeshContentName(store, source);
	store.Set(owner, collider);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	REQUIRE(store.Query<const scene::SplitCollisionCell>().Count() == 2);
	const scene::CollisionShapes *shapes = scene::CollisionShapesOf(store);
	REQUIRE(shapes != nullptr);
	REQUIRE(shapes->FindMesh(collider.Geometry) != nullptr);
	CHECK(shapes->FindMesh(collider.Geometry)->TriangleCount() == 0);
	size_t baked = 0;
	store.Each<const scene::SplitCollisionCell>([&](ecs::Entity cell, const scene::SplitCollisionCell &) {
		const auto *shape = shapes->FindMesh(store.Get<scene::Collider>(cell)->Geometry);
		baked += shape != nullptr && shape->TriangleCount() == 10'000 ? 1 : 0;
	});
	CHECK(baked == 2);

	store.Set(owner, scene::Transform{core::CFrame{core::Vector3{0, 5, 0}}});
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	store.Each<const scene::SplitCollisionCell>([&](ecs::Entity cell, const scene::SplitCollisionCell &) {
		CHECK(store.Get<scene::Transform>(cell)->Frame.Position.Y == Catch::Approx(5.0f));
		// Each cell bounds only itself: 1 unit wide on X around its own centre.
		CHECK(store.Get<scene::Collider>(cell)->Extent.X == Catch::Approx(0.5f));
	});

	store.Set(owner, scene::Motion{});
	REQUIRE_NOTHROW(scheduler.Tick(store, TICK));
	CHECK(store.Query<const scene::SplitCollisionCell>().Count() == 0);
}

TEST_CASE("decorative local hierarchies spend broadphase synchronization work", "[physics][work-budget]") {
	ecs::Store store("work.decorative-hierarchy");
	PreparePhysicsWorld(store);
	const auto instance = ecs::Classes::RegisterInstanceRoot();
	const auto root = store.CreateInstance(instance, "LocalDecoration");
	store.Set(root, ecs::ClientLocal{});
	for (int index = 0; index < 2; ++index) {
		const auto child = store.CreateInstance(instance);
		REQUIRE(store.SetParent(child, root));
	}
	REQUIRE(store.CountMatching<scene::Collider>() == 0);
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 1;
	SetPhysicsWorkLimits(store, limits);
	store.AdvanceTick(TICK);
	BeginPhysicsWorkTick(store);
	REQUIRE(PhysicsWorkBudgetOf(store)->Used == 0);
	REQUIRE_THROWS_AS(SyncBroadphase(store), std::runtime_error);
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(PhysicsWorkBudgetOf(store)->ExhaustedStage == PhysicsWorkStage::BroadphaseSync);
	CHECK(PhysicsWorkBudgetOf(store)->Used == limits.WorkPerTick);
	CHECK(store.Resource<PhysicsWorld>()->StaticRebuilds() == 0);
}

TEST_CASE("one engine tick shares its work across every physics substep", "[physics][work-budget]") {
	ecs::Store reference("work.substeps");
	PreparePhysicsWorld(reference);
	Drift(reference);
	SetPhysicsTickRate(reference, 120.0);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(reference, TICK);
	REQUIRE(PhysicsClockOf(reference)->Steps == 2);
	const uint64_t completeWork = PhysicsWorkBudgetOf(reference)->Used;
	REQUIRE(completeWork > 0);

	ecs::Store limited("work.substeps");
	PreparePhysicsWorld(limited);
	Drift(limited);
	SetPhysicsTickRate(limited, 120.0);
	PhysicsWorkLimits limits;
	limits.WorkPerTick = completeWork - 1;
	SetPhysicsWorkLimits(limited, limits);
	REQUIRE_THROWS_AS(scheduler.Tick(limited, TICK), std::runtime_error);
	CHECK(PhysicsWorkBudgetOf(limited)->Blocked);
	CHECK(PhysicsWorkBudgetOf(limited)->Used <= limits.WorkPerTick);

	ecs::Store exact("work.substeps");
	PreparePhysicsWorld(exact);
	const ecs::Entity body = Drift(exact);
	SetPhysicsTickRate(exact, 120.0);
	limits.WorkPerTick = completeWork;
	SetPhysicsWorkLimits(exact, limits);
	REQUIRE_NOTHROW(scheduler.Tick(exact, TICK));
	CHECK_FALSE(PhysicsWorkBudgetOf(exact)->Blocked);
	CHECK(PhysicsWorkBudgetOf(exact)->Used == completeWork);
	CHECK(exact.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(TICK));
	CHECK(PhysicsClock::MAXIMUM_STEPS_PER_TICK == 8);
}

TEST_CASE("a later refusal stops completed solver output after integration", "[physics][work-budget]") {
	ecs::Store store("work.later-failure");
	ContactScene(store);
	ecs::Entity moving;
	store.Each<const scene::Motion>([&](ecs::Entity entity, const scene::Motion &) { moving = entity; });
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Add(
		"work.limit-after-integration",
		ecs::Phase::Simulation,
		[](ecs::Store &integrated) {
			auto limits = PhysicsWorkBudgetOf(integrated)->Limits;
			limits.WorkPerTick = PhysicsWorkBudgetOf(integrated)->Used;
			SetPhysicsWorkLimits(integrated, limits);
		},
		ecs::SystemOrder{{}, {"physics.simulation"}}
	);
	REQUIRE_THROWS_AS(scheduler.Tick(store, TICK), std::runtime_error);
	CHECK(store.Get<scene::Transform>(moving)->Frame.Position.X == Catch::Approx(TICK));
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	const auto observations = CopyPhysicsObservationRecords(store);
	REQUIRE_FALSE(observations.empty());
	CHECK(std::none_of(observations.begin(), observations.end(), [](const auto &record) {
		return record.Boundary == PhysicsObservationBoundary::CompletedSolver;
	}));
}

TEST_CASE(
	"admitted ticks preserve complete output across serial and worker paths", "[physics][work-budget]"
) {
	ComputeContext restoreCompute;
	WorkerPool pool;
	ecs::Store serial("work.parity");
	ecs::Store threaded("work.parity");
	ContactScene(serial);
	ContactScene(threaded);
	SECTION("contacts") {}
	SECTION("integration crosses the worker dispatch floor") {
		// Eight grains admit the parallel integrator while keeping collider work tiny.
		constexpr size_t MOVING_BODIES = 8 * INTEGRATE_GRAIN;
		for (size_t index = 1; index < MOVING_BODIES; ++index) {
			Drift(serial, {0, 10, 0});
			Drift(threaded, {0, 10, 0});
		}
	}
	// Parity covers the whole world, so no body is parked by the window.
	for (ecs::Store *store : {&serial, &threaded}) {
		PhysicsWorkLimits limits;
		limits.MovingBodies = 1'000'000;
		SetPhysicsWorkLimits(*store, limits);
		PreparedWorkBudgetMutable(*store)->RegionQuota = limits.MovingBodies;
	}
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	for (int tick = 0; tick < 3; ++tick) {
		parallel::SetForceSerialCompute(true);
		scheduler.Tick(serial, TICK);
		parallel::SetForceSerialCompute(false);
		scheduler.Tick(threaded, TICK);
		CHECK(
			physics::testing::SameSnapshot(
				physics::testing::Capture(serial), physics::testing::Capture(threaded)
			)
		);
		CHECK(PhysicsWorkBudgetOf(serial)->Used == PhysicsWorkBudgetOf(threaded)->Used);
		CHECK(Save(serial) == Save(threaded));
	}
	CHECK_FALSE(serial.Resource<PhysicsWorld>()->Manifolds().empty());
}

TEST_CASE("broadphase leases keep exact limits and canonical batch charges", "[physics][work-budget]") {
	ComputeContext restoreCompute;
	WorkerPool pool;
	size_t extraBodies = 0;
	SECTION("one lease boundary") {}
	SECTION("multiple canonical batches") {
		extraBodies = 1024;
	}
	const auto build = [extraBodies](ecs::Store &store) {
		ContactScene(store);
		for (size_t index = 0; index < extraBodies; ++index) {
			const auto body = Drift(store, {4.0f * static_cast<float>(index + 1), 10, 0});
			store.Set(body, scene::Collider{});
		}
		store.AdvanceTick(TICK);
		SyncBroadphase(store);
		BeginPhysicsWorkTick(store);
	};
	ecs::Store serial("work.leases");
	parallel::SetForceSerialCompute(true);
	build(serial);
	const uint64_t admitted = PhysicsWorkBudgetOf(serial)->Used;
	BroadPhase(serial);
	const uint64_t complete = PhysicsWorkBudgetOf(serial)->Used;
	REQUIRE(complete - admitted >= spatial::TraversalBudget::LEASE_UNITS);

	ecs::Store threaded("work.leases");
	parallel::SetForceSerialCompute(false);
	build(threaded);
	BroadPhase(threaded);
	CHECK(PhysicsWorkBudgetOf(threaded)->Used == complete);
	CHECK(
		physics::testing::SameSnapshot(physics::testing::Capture(serial), physics::testing::Capture(threaded))
	);

	uint64_t failedUsed = std::numeric_limits<uint64_t>::max();
	for (const bool forceSerial : {true, false}) {
		CAPTURE(forceSerial);
		parallel::SetForceSerialCompute(forceSerial);
		for (const bool oneShort : {false, true}) {
			CAPTURE(oneShort);
			ecs::Store boundary("work.leases");
			build(boundary);
			auto limits = PhysicsWorkBudgetOf(boundary)->Limits;
			limits.WorkPerTick = complete - static_cast<uint64_t>(oneShort);
			SetPhysicsWorkLimits(boundary, limits);
			if (oneShort) {
				REQUIRE_THROWS_AS(BroadPhase(boundary), std::runtime_error);
				CHECK(PhysicsWorkBudgetOf(boundary)->ExhaustedStage == PhysicsWorkStage::Broadphase);
				if (forceSerial)
					failedUsed = PhysicsWorkBudgetOf(boundary)->Used;
				else
					CHECK(PhysicsWorkBudgetOf(boundary)->Used == failedUsed);
			} else {
				REQUIRE_NOTHROW(BroadPhase(boundary));
				CHECK(PhysicsWorkBudgetOf(boundary)->Used == complete);
				CHECK(
					physics::testing::SameSnapshot(
						physics::testing::Capture(serial), physics::testing::Capture(boundary)
					)
				);
			}
			CHECK(PhysicsWorkBudgetOf(boundary)->Used <= limits.WorkPerTick);
		}
	}
}

TEST_CASE("retired contacts remain budgeted after every collider is removed", "[physics][work-budget]") {
	ecs::Store store("work.retained");
	ContactScene(store);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, TICK);
	auto &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE_FALSE(world.Bodies().empty());
	REQUIRE_FALSE(PipelineInternals::Resting(world).empty());
	REQUIRE_FALSE(PipelineInternals::TouchingLast(world).empty());
	std::vector<ecs::Entity> removed;
	store.EachEntity([&](ecs::Entity entity) { removed.push_back(entity); });
	for (const auto entity : removed)
		store.Destroy(entity);
	store.AdvanceTick(TICK);
	SyncBroadphase(store);
	BroadPhase(store);
	NarrowPhase(store);
	REQUIRE(world.Manifolds().empty());
	REQUIRE(store.CountMatching<scene::Collider>() == 0);

	const auto beforeOwners = PipelineInternals::BodyOwners(world);
	const auto beforeIndex = PipelineInternals::BodyIndexByOwner(world);
	const auto beforeResting = PipelineInternals::Resting(world);
	const auto beforeTouching = PipelineInternals::TouchingLast(world);
	const auto beforeEvents = physics::testing::Capture(store).Events;
	const size_t beforeBodies = world.Bodies().size();
	bool publish = false;
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 0;
	SECTION("retiring solver bodies") {}
	SECTION("carrying old resting rows") {
		limits.WorkPerTick = beforeBodies;
	}
	SECTION("retiring published touching pairs") {
		publish = true;
	}
	SetPhysicsWorkLimits(store, limits);
	BeginPhysicsWorkTick(store);
	if (publish)
		REQUIRE_THROWS_AS(Publish(store), std::runtime_error);
	else
		REQUIRE_THROWS_AS(Solve(store), std::runtime_error);
	CHECK(PhysicsWorkBudgetOf(store)->Blocked);
	CHECK(PhysicsWorkBudgetOf(store)->ExhaustedStage == PhysicsWorkStage::Solver);
	CHECK(PhysicsWorkBudgetOf(store)->Used <= limits.WorkPerTick);
	CHECK(world.Bodies().size() == beforeBodies);
	CHECK(PipelineInternals::BodyOwners(world) == beforeOwners);
	CHECK(PipelineInternals::BodyIndexByOwner(world) == beforeIndex);
	REQUIRE(PipelineInternals::Resting(world).size() == beforeResting.size());
	for (size_t index = 0; index < beforeResting.size(); ++index) {
		const auto &after = PipelineInternals::Resting(world)[index];
		CHECK(after.Owner == beforeResting[index].Owner);
		CHECK(after.RestingSeconds == beforeResting[index].RestingSeconds);
		CHECK(after.Asleep == beforeResting[index].Asleep);
	}
	CHECK(PipelineInternals::TouchingLast(world) == beforeTouching);
	CHECK(physics::testing::Capture(store).Events == beforeEvents);
}

TEST_CASE("inactive standalone integration keeps its original unlimited contract", "[physics][work-budget]") {
	ecs::Store store("work.standalone");
	PreparePhysicsWorld(store);
	const auto body = Drift(store);
	PhysicsWorkLimits limits;
	limits.WorkPerTick = 0;
	limits.MovingBodies = 0;
	SetPhysicsWorkLimits(store, limits);
	SECTION("an installed inactive budget") {}
	SECTION("a legacy prepared store without the resource") {
		store.RemoveResource<PhysicsWorkBudget>();
	}
	store.AdvanceTick(TICK);
	REQUIRE_NOTHROW(IntegrateMotion(store));
	CHECK(store.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(TICK));
	if (PhysicsWorkBudgetOf(store) == nullptr) return;
	CHECK(PhysicsWorkBudgetOf(store)->Used == 0);
	CHECK_FALSE(PhysicsWorkBudgetOf(store)->Blocked);
	REQUIRE_THROWS_AS(BeginPhysicsWorkTick(store), std::runtime_error);
}

TEST_CASE("checkpoints preserve completed and blocked work state", "[physics][work-budget][snapshot]") {
	bool legacy = false;
	bool configured = false;
	SECTION("complete and failed modern checkpoints") {}
	SECTION("a legacy checkpoint seeds default limits on its first scheduled tick") {
		legacy = true;
	}
	SECTION("explicit limits apply before a legacy checkpoint's first scheduled tick") {
		legacy = true;
		configured = true;
	}
	ecs::Store source("work.checkpoint");
	PreparePhysicsWorld(source);
	const ecs::Entity body = Drift(source);
	ecs::Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(source, TICK);
	const auto good = Save(source);
	ecs::Store restored("work.checkpoint");
	if (legacy) {
		source.RemoveResource<PhysicsWorkBudget>();
		Load(restored, Save(source));
		CHECK(PhysicsWorkBudgetOf(restored) == nullptr);
		if (configured) {
			PhysicsWorkLimits limits;
			limits.MovingBodies = 0;
			SetPhysicsWorkLimits(restored, limits);
			REQUIRE(PhysicsWorkBudgetOf(restored));
			CHECK(PhysicsWorkBudgetOf(restored)->Limits.MovingBodies == 0);
			REQUIRE_THROWS_AS(scheduler.Tick(restored, TICK), std::runtime_error);
			CHECK(restored.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(TICK));
			CHECK(PhysicsWorkBudgetOf(restored)->Blocked);
			CHECK(PhysicsWorkBudgetOf(restored)->ExhaustedStage == PhysicsWorkStage::Admission);
		} else {
			REQUIRE_NOTHROW(scheduler.Tick(restored, TICK));
			REQUIRE(PhysicsWorkBudgetOf(restored));
			CHECK(PhysicsWorkBudgetOf(restored)->Used > 0);
			CHECK_FALSE(PhysicsWorkBudgetOf(restored)->Blocked);
			CHECK(restored.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(2 * TICK));
		}
		return;
	}
	Load(restored, good);
	CHECK(Save(restored) == good);
	REQUIRE_NOTHROW(scheduler.Tick(restored, TICK));
	CHECK(restored.Get<scene::Transform>(body)->Frame.Position.X == Catch::Approx(2 * TICK));

	source.AdvanceTick(TICK);
	BeginPhysicsWorkTick(source);
	REQUIRE_THROWS_AS(FailPhysicsWork(source, PhysicsWorkStage::Narrowphase), std::runtime_error);
	const auto blocked = Save(source);
	Load(restored, blocked);
	CHECK(PhysicsWorkBudgetOf(restored)->Blocked);
	CHECK(PhysicsWorkBudgetOf(restored)->ExhaustedStage == PhysicsWorkStage::Narrowphase);
	REQUIRE_THROWS_AS(scheduler.Tick(restored, TICK), std::runtime_error);
	Load(restored, good);
	REQUIRE_NOTHROW(scheduler.Tick(restored, TICK));
	CHECK_FALSE(PhysicsWorkBudgetOf(restored)->Blocked);

	ecs::Store active("work.checkpoint");
	Load(active, good);
	active.AdvanceTick(TICK);
	BeginPhysicsWorkTick(active);
	IntegrateMotion(active);
	Load(restored, Save(active));
	CHECK(PhysicsWorkBudgetOf(restored)->Blocked);
	CHECK_FALSE(PhysicsWorkActive(restored));
	REQUIRE_THROWS_AS(scheduler.Tick(restored, TICK), std::runtime_error);
}

TEST_CASE(
	"work state decoding refuses malformed batches transactionally", "[physics][work-budget][snapshot]"
) {
	RegisterPhysicsComponents();
	const auto id = ecs::Components::Find(core::Name("physics.PhysicsWorkBudget"));
	REQUIRE(id.IsValid());
	const auto &descriptor = ecs::Components::Describe(id);
	REQUIRE(descriptor.Write != nullptr);
	REQUIRE(descriptor.Read != nullptr);
	PhysicsWorkBudget invalid;
	bool truncate = false;
	bool wrongVersion = false;
	SECTION("a truncated second record") {
		truncate = true;
	}
	SECTION("an unknown version") {
		wrongVersion = true;
	}
	SECTION("an unknown stage") {
		invalid.ExhaustedStage = static_cast<PhysicsWorkStage>(255);
	}
	SECTION("a failure stage without a latch") {
		invalid.ExhaustedStage = PhysicsWorkStage::Solver;
	}
	std::array<PhysicsWorkBudget, 2> source{};
	source[0].Used = 3;
	source[1] = invalid;
	core::ByteWriter writer;
	descriptor.Write(writer, source.data(), source.size());
	std::array<PhysicsWorkBudget, 2> destination{};
	destination[0].Used = 17;
	destination[1].Used = 19;
	std::vector<std::byte> bytes(writer.Bytes().begin(), writer.Bytes().end());
	if (truncate) bytes.pop_back();
	if (wrongVersion) bytes[0] = std::byte{255};
	core::ByteReader reader(bytes);
	descriptor.Read(reader, destination.data(), destination.size());
	CHECK(reader.Failed());
	CHECK(destination[0].Used == 17);
	CHECK(destination[1].Used == 19);
}
