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
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Starting per-region quota for the adaptive window.
	inline constexpr uint64_t INITIAL_REGION_QUOTA = 256;

	// Policy defaults, chosen to cap one tick without a machine-dependent clock.
	// @since v0.26.1
	struct PhysicsWorkLimits {
		// Work units shared by every substep in one world tick.
		uint64_t WorkPerTick = 16'000'000;
		// Moving bodies simulated in one tick within one region. A region with
		// more moves a rotating window of this many; see
		// `PhysicsWorkBudget::WindowTurn`. A spread-out world therefore
		// simulates more bodies a tick than a packed one.
		uint64_t MovingBodies = 4'096;
		// Edge of one cubic window region, in metres.
		uint64_t RegionMetres = 64;
		// All collider rows admitted before any physics work. Colliders are
		// resident data; the broad phase they feed is bounded by `WorkPerTick`.
		uint64_t Colliders = 262'144;
		// Combined JointInstance and WeldConstraint rows.
		uint64_t Joints = 4'096;
		// Local bodies carrying copied contact geometry.
		uint64_t CopiedBodies = 4'096;
		// Editable mesh rows admitted before collision refresh.
		uint64_t EditableMeshes = 1'024;
		// Total catalogue, geometry and editable array elements. The bundled
		// Terrain demo keeps up to 81 streamed chunks of about 90,000 elements
		// each (render arrays and baked collision), so this leaves it 2x room.
		uint64_t GeometryElements = 16'000'000;
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

		// Rotating admission. Moving bodies are grouped by the region their
		// position falls in; a region holding more than `Limits.MovingBodies`
		// simulates that many a tick, taken in id order from an offset that
		// advances with `WindowTurn`, so every body gets a turn. A body left
		// out is parked for the tick: not integrated, no applied loads, and
		// fixed in the solver like a sleeper, while active bodies still collide
		// with it. Humanoid root bodies are always active. Only the turn persists.
		uint64_t WindowTurn = 0;
		// Bodies each region may simulate next tick, at most
		// `Limits.MovingBodies`. It starts at `INITIAL_REGION_QUOTA`, grows while
		// ticks use under half of `WorkPerTick` and shrinks above three quarters,
		// so a world that would exceed the work cap slows down instead.
		uint64_t RegionQuota = 0;
		// Derived at admission for the tick in progress: whether any body is
		// parked, and the sorted ids of every active moving body.
		//@{
		bool Windowed = false;
		std::vector<uint64_t> Active;
		//@}
	};

	// Reads status without registering or mutating an unprepared store.
	// @threadsafe
	const PhysicsWorkBudget *PhysicsWorkBudgetOf(const ecs::Store &store);

	// Changes future admission limits without clearing an existing failure latch.
	void SetPhysicsWorkLimits(ecs::Store &store, const PhysicsWorkLimits &limits);
}
