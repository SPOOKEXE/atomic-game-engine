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
	// Discovery name for observations after narrow phase and before solving.
	inline constexpr std::string_view PHYSICS_PRE_SOLVE_OBSERVATION = "physics.pre-solve";
	// Discovery name for observations after solving and before publication.
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

	// Combines independent observation availability flags.
	constexpr PhysicsObservationAvailability
	operator|(PhysicsObservationAvailability left, PhysicsObservationAvailability right) {
		return static_cast<PhysicsObservationAvailability>(
			static_cast<uint16_t>(left) | static_cast<uint16_t>(right)
		);
	}

	// Reports whether an observation availability flag is present.
	constexpr bool
	HasObservationValue(PhysicsObservationAvailability values, PhysicsObservationAvailability value) {
		return (static_cast<uint16_t>(values) & static_cast<uint16_t>(value)) != 0;
	}

	// The immutable identity every hook context shares. `World` is copied into
	// the completed record, so an observation never transports a Store pointer.
	struct PhysicsObservationIdentity {
		// Stable discovery name of the observed boundary.
		std::string_view Hook;
		// Stable name of the observed world.
		std::string_view World;
		// World tick that contains this observation.
		uint64_t Tick = 0;
		// Physics step number when the clock supplies one.
		uint64_t PhysicsStep = 0;
		// Step ordinal within Tick when available.
		uint32_t StepInTick = 0;
		// Duration of the physics step.
		float StepSeconds = 0.0f;
		// Unit used by StepSeconds.
		PhysicsObservationTimeUnit TimeUnit = PhysicsObservationTimeUnit::Seconds;
		// Whether PhysicsStep identifies this record.
		bool PhysicsStepAvailable = false;
		// Whether StepInTick identifies this record.
		bool StepInTickAvailable = false;
	};

	// Read-only input available immediately after `IntegrateMotion`.
	struct PostIntegrationObservationContext {
		// Shared immutable boundary identity.
		const PhysicsObservationIdentity Identity;
		// Bodies integrated at this boundary.
		const size_t MovingBodies;
	};

	// Read-only input available after `NarrowPhase` and before `Solve`.
	struct PreSolveObservationContext {
		// Shared immutable boundary identity.
		const PhysicsObservationIdentity Identity;
		// Broad-phase candidates entering narrow phase.
		const size_t CandidatePairs;
		// Contact manifolds entering the solver.
		const size_t Manifolds;
	};

	// Read-only input available after `Solve` and before `Publish`.
	struct CompletedSolverObservationContext {
		// Shared immutable boundary identity.
		const PhysicsObservationIdentity Identity;
		// Bodies processed by the solver.
		const size_t SolverBodies;
		// Constraint rows processed by the solver.
		const size_t SolverRows;
	};

	// A completed copy from one declared observation point. Counts are deliberately
	// summaries: manifold, body, and ECS storage remain private to the physics
	// world and cannot be mutated through an observation.
	struct PhysicsObservationRecord {
		// DataFactory world ids permit 128 UTF-8 bytes. One extra byte preserves
		// the terminating zero in this fixed, allocation-free record.
		static constexpr size_t MAXIMUM_WORLD_NAME_BYTES = 129;

		// Pipeline boundary described by this record.
		PhysicsObservationBoundary Boundary = PhysicsObservationBoundary::PostIntegration;
		// Mask of fields with meaningful values.
		PhysicsObservationAvailability Available = PhysicsObservationAvailability::None;
		// Null-terminated world name copied into the record.
		std::array<char, MAXIMUM_WORLD_NAME_BYTES> World = {};
		// World tick containing the boundary.
		uint64_t Tick = 0;
		// Physics step number when supplied.
		uint64_t PhysicsStep = 0;
		// Step ordinal within Tick when supplied.
		uint32_t StepInTick = 0;
		// Physics step duration.
		float StepSeconds = 0.0f;
		// Count of integrated moving bodies.
		uint32_t MovingBodies = 0;
		// Count of broad-phase candidate pairs.
		uint32_t CandidatePairs = 0;
		// Count of narrow-phase manifolds.
		uint32_t Manifolds = 0;
		// Count of solver bodies.
		uint32_t SolverBodies = 0;
		// Count of solver constraint rows.
		uint32_t SolverRows = 0;
		// Unit used by StepSeconds.
		PhysicsObservationTimeUnit TimeUnit = PhysicsObservationTimeUnit::Seconds;

		// Returns this boundary's stable hook name.
		std::string_view Hook() const;
		// Returns the copied stable world name.
		std::string_view WorldName() const;
	};

	// Per-world, overwrite-on-full storage. Its fixed capacity keeps observation
	// from becoming a backlog when a consumer is absent or slow.
	class PhysicsObservationLog {
	  public:
		// Maximum completed records retained for one world.
		static constexpr size_t CAPACITY = 96;

		// Copies retained records in production order.
		std::vector<PhysicsObservationRecord> Copy() const;
		// Returns records overwritten after the ring became full.
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
