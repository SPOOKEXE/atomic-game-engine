#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Observation.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.physics.observation")
TEST_DEPENDS("engine.physics.pipeline")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::physics::CopyPhysicsObservationRecords;
using engine::physics::HasObservationValue;
using engine::physics::OverwrittenPhysicsObservationRecords;
using engine::physics::PHYSICS_COMPLETED_SOLVER_OBSERVATION;
using engine::physics::PHYSICS_POST_INTEGRATION_OBSERVATION;
using engine::physics::PHYSICS_PRE_SOLVE_OBSERVATION;
using engine::physics::PhysicsClock;
using engine::physics::PhysicsObservationAvailability;
using engine::physics::PhysicsObservationBoundary;
using engine::physics::PhysicsObservationHooks;
using engine::physics::PhysicsObservationRecord;
using engine::physics::PreparePhysicsWorld;
using engine::physics::RegisterPhysicsSystems;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::Transform;

namespace {
	void AddBox(Store &store, Vector3 position, Vector3 velocity) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{position}});
		store.Set<Motion>(entity, Motion{velocity, Vector3::Zero});
		Collider collider;
		collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
		store.Set<Collider>(entity, collider);
	}

	const PhysicsObservationRecord &
	At(const std::vector<PhysicsObservationRecord> &records, PhysicsObservationBoundary boundary) {
		const auto found = std::find_if(records.begin(), records.end(), [boundary](const auto &record) {
			return record.Boundary == boundary;
		});
		REQUIRE(found != records.end());
		return *found;
	}
}

TEST_CASE(
	"physics observations expose one immutable summary at each fixed-step boundary", "[physics][observation]"
) {
	Store store{"physics-observation-world"};
	PreparePhysicsWorld(store, 1.0f);
	AddBox(store, Vector3{-0.4f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f});
	AddBox(store, Vector3{0.4f, 0.0f, 0.0f}, Vector3{-1.0f, 0.0f, 0.0f});

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0f / 60.0f);

	const std::vector<PhysicsObservationRecord> records = CopyPhysicsObservationRecords(store);
	REQUIRE(records.size() == 3);
	const PhysicsObservationRecord &integrated = At(records, PhysicsObservationBoundary::PostIntegration);
	const PhysicsObservationRecord &preSolve = At(records, PhysicsObservationBoundary::PreSolve);
	const PhysicsObservationRecord &solved = At(records, PhysicsObservationBoundary::CompletedSolver);

	CHECK(integrated.Hook() == PHYSICS_POST_INTEGRATION_OBSERVATION);
	CHECK(preSolve.Hook() == PHYSICS_PRE_SOLVE_OBSERVATION);
	CHECK(solved.Hook() == PHYSICS_COMPLETED_SOLVER_OBSERVATION);
	for (const PhysicsObservationRecord &record : records) {
		CHECK(record.WorldName() == "physics-observation-world");
		CHECK(record.Tick == 1);
		CHECK(record.PhysicsStep == 1);
		CHECK(record.StepInTick == 1);
		CHECK(record.StepSeconds == Approx(1.0f / 60.0f));
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::World));
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::Tick));
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::PhysicsStep));
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::StepInTick));
	}
	CHECK(integrated.MovingBodies == 2);
	CHECK(HasObservationValue(integrated.Available, PhysicsObservationAvailability::MovingBodies));
	CHECK(preSolve.CandidatePairs == 1);
	CHECK(preSolve.Manifolds == 1);
	CHECK(HasObservationValue(preSolve.Available, PhysicsObservationAvailability::Manifolds));
	CHECK(solved.SolverBodies == 2);
	CHECK(HasObservationValue(solved.Available, PhysicsObservationAvailability::SolverRows));
	CHECK(solved.SolverRows == 0);
}

TEST_CASE("physics observations declare stable discovery names", "[physics][observation]") {
	const auto hooks = PhysicsObservationHooks();
	REQUIRE(hooks.size() == 3);
	CHECK(hooks[0] == "physics.post-integration");
	CHECK(hooks[1] == "physics.pre-solve");
	CHECK(hooks[2] == "physics.completed-solver");
}

TEST_CASE(
	"physics observation storage overwrites old completed records at its fixed bound",
	"[physics][observation]"
) {
	Store store{"physics-observation-bound"};
	PreparePhysicsWorld(store);
	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	for (uint64_t tick = 0; tick < 33; tick++) {
		scheduler.Tick(store, 1.0f / 60.0f);
	}

	const std::vector<PhysicsObservationRecord> records = CopyPhysicsObservationRecords(store);
	REQUIRE(records.size() == 96);
	CHECK(records.front().Tick == 2);
	CHECK(records.back().Tick == 33);
	CHECK(OverwrittenPhysicsObservationRecords(store) == 3);
}

TEST_CASE("physics observations retain a 128-byte DataFactory world id exactly", "[physics][observation]") {
	const std::string world(128, 'w');
	Store store{world};
	PreparePhysicsWorld(store);
	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0f / 60.0f);

	const std::vector<PhysicsObservationRecord> records = CopyPhysicsObservationRecords(store);
	REQUIRE(records.size() == 3);
	for (const PhysicsObservationRecord &record : records) {
		CHECK(record.WorldName() == world);
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::World));
	}
}

TEST_CASE(
	"physics observations retain tick identity when a world name exceeds the fixed record bound",
	"[physics][observation]"
) {
	Store store{std::string(PhysicsObservationRecord::MAXIMUM_WORLD_NAME_BYTES, 'w')};
	PreparePhysicsWorld(store);
	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0f / 60.0f);

	const std::vector<PhysicsObservationRecord> records = CopyPhysicsObservationRecords(store);
	REQUIRE(records.size() == 3);
	for (const PhysicsObservationRecord &record : records) {
		CHECK(record.Tick == 1);
		CHECK(record.PhysicsStep == 1);
		CHECK_FALSE(HasObservationValue(record.Available, PhysicsObservationAvailability::World));
	}
}

TEST_CASE(
	"physics observations mark clock-derived identity unavailable without a physics clock",
	"[physics][observation]"
) {
	Store store{"physics-observation-no-clock"};
	PreparePhysicsWorld(store);
	store.RemoveResource<PhysicsClock>();
	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, 1.0f / 60.0f);

	const std::vector<PhysicsObservationRecord> records = CopyPhysicsObservationRecords(store);
	REQUIRE(records.size() == 3);
	for (const PhysicsObservationRecord &record : records) {
		CHECK(record.Tick == 1);
		CHECK_FALSE(HasObservationValue(record.Available, PhysicsObservationAvailability::PhysicsStep));
		CHECK_FALSE(HasObservationValue(record.Available, PhysicsObservationAvailability::StepInTick));
		CHECK(HasObservationValue(record.Available, PhysicsObservationAvailability::StepSeconds));
	}
}
