#pragma once

// Deterministic work limits for the scheduled physics pipeline. These counts
// bound admitted work and storage; they are not a wall-clock time guarantee.
// Units count weighted bounded kernels and raw traversal visits, not CPU
// instructions. The default limits are policy, without a measured time claim.
// Direct queries and stages outside the scheduled context stay unlimited.
// Exhaustion can follow partial integration, so recovery requires recreating
// the store or restoring a complete snapshot taken before the failed tick.
//
// @tier L8 · shared

#include <cstdint>
#include <limits>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Policy defaults, chosen to cap one tick without a machine-dependent clock.
	// @since v0.26.1
	struct PhysicsWorkLimits {
		// Work units shared by every substep in one world tick.
		uint64_t WorkPerTick = 1'000'000;
		// Motion and Transform rows admitted before integration.
		uint64_t MovingBodies = 16'384;
		// All collider rows admitted before any physics work.
		uint64_t Colliders = 65'536;
		// Combined JointInstance and WeldConstraint rows.
		uint64_t Joints = 4'096;
		// Local bodies carrying copied contact geometry.
		uint64_t CopiedBodies = 4'096;
		// Editable mesh rows admitted before collision refresh.
		uint64_t EditableMeshes = 1'024;
		// Total catalogue, geometry and editable array elements.
		uint64_t GeometryElements = 1'000'000;
		// Generated grid entries admitted across every rebuild in one tick.
		uint64_t GridEntries = 1'000'000;
	};

	// The owner-thread stage that refused further work.
	enum class PhysicsWorkStage : uint8_t {
		None,
		Admission,
		EditableMesh,
		Integration,
		BroadphaseSync,
		Broadphase,
		Continuous,
		Narrowphase,
		Solver,
		Welds,
		CopiedContacts,
	};

	// Persistent limits and last-tick status. Blocked is a sticky failure latch.
	struct PhysicsWorkBudget {
		// Limits used for the next admission and remaining work.
		PhysicsWorkLimits Limits;
		// World tick that owns the counters below.
		uint64_t Tick = std::numeric_limits<uint64_t>::max();
		// Work reserved before execution, including unused worker lease tails.
		uint64_t Used = 0;
		// Generated spatial entries admitted across every rebuild and substep.
		uint64_t GridEntriesUsed = 0;
		// Terminal latch after a failed work reservation.
		bool Blocked = false;
		// An incomplete snapshot restores as blocked rather than resuming partial work.
		bool InProgress = false;
		// First stage that refused further work.
		PhysicsWorkStage ExhaustedStage = PhysicsWorkStage::None;
	};

	// Reads status without registering or mutating an unprepared store.
	// @threadsafe
	const PhysicsWorkBudget *PhysicsWorkBudgetOf(const ecs::Store &store);

	// Changes future admission limits without clearing an existing failure latch.
	void SetPhysicsWorkLimits(ecs::Store &store, const PhysicsWorkLimits &limits);
}
