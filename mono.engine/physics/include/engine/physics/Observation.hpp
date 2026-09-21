#pragma once

// Fixed-boundary, read-only observations of a physics step.
//
// These are records rather than callbacks. A callback would run arbitrary host
// code in the solver's tick and could retain or mutate world state. The pipeline
// instead copies a small summary into a bounded world resource, and consumers
// take completed value copies after the tick.
//
// @tier L8 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Stable discovery names. These strings, rather than their enum values,
	// identify observations outside this process.
	inline constexpr std::string_view PHYSICS_POST_INTEGRATION_OBSERVATION = "physics.post-integration";
	inline constexpr std::string_view PHYSICS_PRE_SOLVE_OBSERVATION = "physics.pre-solve";
	inline constexpr std::string_view PHYSICS_COMPLETED_SOLVER_OBSERVATION = "physics.completed-solver";

	// The complete discovery surface in deterministic pipeline order.
	std::span<const std::string_view> PhysicsObservationHooks();

	// The declared point in a fixed physics step that a record describes.
	enum class PhysicsObservationBoundary : uint8_t {
		// Transforms after integration and before the broad phase rebuild.
		PostIntegration,
		// Candidate pairs and manifolds after narrow phase, before Solve.
		PreSolve,
		// Solver bodies and rows after Solve, before Publish changes ECS rows.
		CompletedSolver,
	};

	// Units used by `PhysicsObservationRecord::StepSeconds`.
	enum class PhysicsObservationTimeUnit : uint8_t {
		Seconds,
	};

	// Which optional values are valid in one record. A consumer must inspect this
	// mask instead of treating zero as an unavailable count.
	enum class PhysicsObservationAvailability : uint16_t {
		None = 0,
		World = 1 << 0,
		Tick = 1 << 1,
		PhysicsStep = 1 << 2,
		StepInTick = 1 << 3,
		StepSeconds = 1 << 4,
		MovingBodies = 1 << 5,
		CandidatePairs = 1 << 6,
		Manifolds = 1 << 7,
		SolverBodies = 1 << 8,
		SolverRows = 1 << 9,
	};

	constexpr PhysicsObservationAvailability
	operator|(PhysicsObservationAvailability left, PhysicsObservationAvailability right) {
		return static_cast<PhysicsObservationAvailability>(
			static_cast<uint16_t>(left) | static_cast<uint16_t>(right)
		);
	}

	constexpr bool
	HasObservationValue(PhysicsObservationAvailability values, PhysicsObservationAvailability value) {
		return (static_cast<uint16_t>(values) & static_cast<uint16_t>(value)) != 0;
	}

	// The immutable identity every hook context shares. `World` is copied into
	// the completed record, so an observation never transports a Store pointer.
	struct PhysicsObservationIdentity {
		std::string_view Hook;
		std::string_view World;
		uint64_t Tick = 0;
		uint64_t PhysicsStep = 0;
		uint32_t StepInTick = 0;
		float StepSeconds = 0.0f;
		PhysicsObservationTimeUnit TimeUnit = PhysicsObservationTimeUnit::Seconds;
		bool PhysicsStepAvailable = false;
		bool StepInTickAvailable = false;
	};

	// Read-only input available immediately after `IntegrateMotion`.
	struct PostIntegrationObservationContext {
		const PhysicsObservationIdentity Identity;
		const size_t MovingBodies;
	};

	// Read-only input available after `NarrowPhase` and before `Solve`.
	struct PreSolveObservationContext {
		const PhysicsObservationIdentity Identity;
		const size_t CandidatePairs;
		const size_t Manifolds;
	};

	// Read-only input available after `Solve` and before `Publish`.
	struct CompletedSolverObservationContext {
		const PhysicsObservationIdentity Identity;
		const size_t SolverBodies;
		const size_t SolverRows;
	};

	// A completed copy from one declared observation point. Counts are deliberately
	// summaries: manifold, body, and ECS storage remain private to the physics
	// world and cannot be mutated through an observation.
	struct PhysicsObservationRecord {
		// DataFactory world ids permit 128 UTF-8 bytes. One extra byte preserves
		// the terminating zero in this fixed, allocation-free record.
		static constexpr size_t MAXIMUM_WORLD_NAME_BYTES = 129;

		PhysicsObservationBoundary Boundary = PhysicsObservationBoundary::PostIntegration;
		PhysicsObservationAvailability Available = PhysicsObservationAvailability::None;
		std::array<char, MAXIMUM_WORLD_NAME_BYTES> World = {};
		uint64_t Tick = 0;
		uint64_t PhysicsStep = 0;
		uint32_t StepInTick = 0;
		float StepSeconds = 0.0f;
		uint32_t MovingBodies = 0;
		uint32_t CandidatePairs = 0;
		uint32_t Manifolds = 0;
		uint32_t SolverBodies = 0;
		uint32_t SolverRows = 0;
		PhysicsObservationTimeUnit TimeUnit = PhysicsObservationTimeUnit::Seconds;

		std::string_view Hook() const;
		std::string_view WorldName() const;
	};

	// Per-world, overwrite-on-full storage. Its fixed capacity keeps observation
	// from becoming a backlog when a consumer is absent or slow.
	class PhysicsObservationLog {
	  public:
		static constexpr size_t CAPACITY = 96;

		std::vector<PhysicsObservationRecord> Copy() const;
		uint64_t Overwritten() const;

	  private:
		friend void RecordPostIntegration(ecs::Store &store);
		friend void RecordPreSolve(ecs::Store &store);
		friend void RecordCompletedSolver(ecs::Store &store);

		void Append(const PhysicsObservationRecord &record);

		std::array<PhysicsObservationRecord, CAPACITY> Records = {};
		size_t First = 0;
		size_t Count = 0;
		uint64_t OverwrittenRecords = 0;
	};

	// Returns completed observations in production order as values. The caller
	// owns the returned records and receives no access to physics state.
	std::vector<PhysicsObservationRecord> CopyPhysicsObservationRecords(const ecs::Store &store);

	// Number of completed records overwritten because this world's fixed ring
	// was full. The counter is monotonic until the world is prepared again.
	uint64_t OverwrittenPhysicsObservationRecords(const ecs::Store &store);
}
