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

	const char *Describe(ExecutionQueue queue);

	// Whether work on a separate device queue may overlap another branch.
	enum class AsyncPolicy : uint8_t {
		Automatic,
		Allow,
		Serial,
	};

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

	const char *Describe(CullingMode mode);

	// One node after dependency and queue classification.
	struct ScheduledNode {
		NodeId Node;
		ExecutionQueue Queue = ExecutionQueue::Graphics;
		AsyncPolicy Async = AsyncPolicy::Automatic;
		CullingMode Culling = CullingMode::Inherit;

		// Compute workgroups. Raster, CPU, and transfer nodes leave these at one.
		uint32_t GroupsX = 1;
		uint32_t GroupsY = 1;
		uint32_t GroupsZ = 1;

		// Whether this node may overlap another queue in its wave.
		bool AsyncEligible = false;
	};

	// Nodes in one wave have no dependency on one another.
	struct ExecutionWave {
		std::vector<ScheduledNode> Nodes;

		// True when the wave contains work for more than one queue. This is
		// permission to overlap, not a requirement imposed on the backend.
		bool Concurrent = false;
	};

	struct ExecutionSchedule {
		std::vector<ExecutionWave> Waves;

		void Clear() {
			Waves.clear();
		}
	};

	enum class ScheduleStatus : uint8_t {
		Ok,
		InvalidGraph,
		MissingProducer,
		ScopeDependency,
		Cycle,
		InvalidHint,
	};

	const char *Describe(ScheduleStatus status);

	// Derives data dependencies, a stable topological order, and independent
	// execution waves. Disabled nodes do not participate.
	ScheduleStatus CompileSchedule(const RenderGraph &graph, ExecutionSchedule &out, core::Name &offender);
}
