#pragma once

// Compiles an authored render graph into dependency waves and device queues.
//
// A canvas is free to place a producer below its consumer. The connection is
// the truth, so a resource with one producer is ordered from that producer to
// every reader regardless of box position. Resources with several writers are
// versioned by declaration order because a read-modify-write chain has no
// other unambiguous order.
//
// The result is deliberately device-free. A backend with async compute may
// submit independent compute and graphics work in one wave concurrently. A
// backend with one queue walks the same wave serially and remains correct.
//
// @tier L9 · shared

#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <vector>

namespace engine::graph {

	// Which sort of device work a node produces.
	enum class ExecutionQueue : uint8_t {
		Cpu,
		Graphics,
		Compute,
		Transfer,
	};

	// One line naming a queue, for a message a person reads.
	//
	// @param queue The queue.
	// @return Its name. Never null, and never empty.
	const char *Describe(ExecutionQueue queue);

	// Whether work on a separate device queue may overlap another branch.
	enum class AsyncPolicy : uint8_t {
		Automatic,
		Allow,
		Serial,
	};

	// One line naming a policy, for a message a person reads.
	//
	// @param policy The policy.
	// @return Its name. Never null, and never empty.
	const char *Describe(AsyncPolicy policy);

	// Which visibility domain a draw pass asks the renderer to use. Dedicated
	// entity-flow nodes remain the composable form; this is the pass-level
	// override for work that owns its draw-list construction.
	enum class CullingMode : uint8_t {
		Inherit,
		None,
		Frustum,
		Occlusion,
	};

	// One line naming a culling mode, for a message a person reads.
	//
	// @param mode The mode.
	// @return Its name. Never null, and never empty.
	const char *Describe(CullingMode mode);

	// One node after dependency and queue classification.
	struct ScheduledNode {
		// The authored node this classification is about.
		NodeId Node;

		// Which queue its work belongs on, decided here rather than by the
		// backend so the schedule reads the same on a device with one queue and
		// a device with three.
		ExecutionQueue Queue = ExecutionQueue::Graphics;

		// What the author asked for about overlapping. `AsyncEligible` below is
		// what the scheduler concluded, and the two are deliberately separate:
		// asking is not the same as being allowed.
		AsyncPolicy Async = AsyncPolicy::Automatic;

		// Which visibility domain a draw pass asked for, or `Inherit` to take
		// the one the frame is already using.
		CullingMode Culling = CullingMode::Inherit;

		// Compute workgroups. Raster, CPU, and transfer nodes leave these at one.
		//@{
		uint32_t GroupsX = 1;
		uint32_t GroupsY = 1;
		uint32_t GroupsZ = 1;
		//@}

		// Whether this node may overlap another queue in its wave.
		bool AsyncEligible = false;
	};

	// Nodes in one wave have no dependency on one another.
	struct ExecutionWave {
		// The nodes in this wave. Order within a wave carries no meaning, which
		// is what "no dependency on one another" amounts to.
		std::vector<ScheduledNode> Nodes;

		// True when the wave contains work for more than one queue. This is
		// permission to overlap, not a requirement imposed on the backend.
		bool Concurrent = false;
	};

	// A whole graph reduced to waves that must run in order.
	//
	// This is the device-free half: it says what depends on what and which work
	// could overlap. `FrameExecutionPlan` is what turns it into the invocations
	// and the traffic one frame actually has.
	struct ExecutionSchedule {
		// The waves, in the order they must run. Everything in one wave may run
		// at once; nothing in wave N may start before wave N-1 has finished.
		std::vector<ExecutionWave> Waves;

		// Empties the schedule so one instance can be recompiled in place.
		void Clear() {
			Waves.clear();
		}
	};

	// Why a graph could not be scheduled.
	//
	// **Named rather than a bool**, because each of these is a mistake in an
	// authored graph and `CompileSchedule` hands back the offending node beside
	// it - a person needs both to know what to change.
	enum class ScheduleStatus : uint8_t {
		// The schedule was produced.
		Ok,

		// The graph itself did not hold together well enough to walk.
		InvalidGraph,

		// A node reads a resource nothing writes, so its input would be
		// whatever the last frame left behind.
		MissingProducer,

		// A dependency crosses a scope that cannot carry it - a frame node
		// waiting on something produced once per view has no single answer to
		// wait for.
		ScopeDependency,

		// The dependencies form a loop, so no order exists.
		Cycle,

		// An authored hint contradicts itself, such as demanding a queue the
		// node's own work cannot run on.
		InvalidHint,
	};

	// One line naming a status, for a message a person reads.
	//
	// @param status What went wrong.
	// @return Its name. Never null, and never empty.
	const char *Describe(ScheduleStatus status);

	// Derives data dependencies, a stable topological order, and independent
	// execution waves. Disabled nodes do not participate.
	ScheduleStatus CompileSchedule(const RenderGraph &graph, ExecutionSchedule &out, core::Name &offender);
}
