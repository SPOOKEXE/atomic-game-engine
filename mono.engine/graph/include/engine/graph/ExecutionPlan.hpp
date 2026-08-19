#pragma once

// Expands a scheduled graph into the concrete work and resource traffic for one frame.
//
// The schedule says which authored nodes may share a wave. This plan adds the
// multiplicity that only a frame knows: one world node per distinct world, one
// view node per view, and one frame node. It remains device-free so Studio and
// every renderer backend use the same accounting and queue barriers.
//
// @tier L9 · shared

#include <engine/graph/Schedule.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph {

	// One scheduled node for one logical resource domain.
	struct PlannedInvocation {
		ScheduledNode Scheduled;
		NodeScope Scope = NodeScope::View;

		// View and world positions use RunContext::WHOLE_FRAME when they do not
		// apply. WorldKey is the caller's stable value, while World is its dense
		// first-appearance position for this frame.
		size_t View = RunContext::WHOLE_FRAME;
		size_t World = RunContext::WHOLE_FRAME;
		uint64_t WorldKey = 0;

		uint64_t ReadBytes = 0;
		uint64_t WriteBytes = 0;
	};

	// Concrete invocations derived from one dependency wave.
	struct PlannedWave {
		std::vector<PlannedInvocation> Invocations;
		bool ConcurrentQueues = false;
		bool IndependentWorlds = false;
		bool IndependentViews = false;
	};

	// A resource ownership handoff between device queues.
	//
	// Bytes describes the resource range made visible. It is synchronization
	// traffic, not necessarily a physical memory copy.
	struct QueueTransfer {
		ResourceId Resource;
		NodeId Producer;
		NodeId Consumer;
		ExecutionQueue From = ExecutionQueue::Graphics;
		ExecutionQueue To = ExecutionQueue::Graphics;
		size_t View = RunContext::WHOLE_FRAME;
		size_t World = RunContext::WHOLE_FRAME;
		uint64_t Bytes = 0;
	};

	struct FrameExecutionPlan {
		std::vector<PlannedWave> Waves;
		std::vector<QueueTransfer> Transfers;
		uint64_t ReadBytes = 0;
		uint64_t WriteBytes = 0;
		uint64_t QueueTransferBytes = 0;

		void Clear() {
			Waves.clear();
			Transfers.clear();
			ReadBytes = 0;
			WriteBytes = 0;
			QueueTransferBytes = 0;
		}
	};

	enum class ExecutionPlanStatus : uint8_t {
		Ok,
		InvalidDimensions,
		MissingNode,
		MissingResource,
		InvalidScopeAccess,
	};

	const char *Describe(ExecutionPlanStatus status);

	// Expands a valid schedule for one frame and measures its resource traffic.
	//
	// Worlds contains one stable world key per view. Repeated keys share
	// world-scoped resources. An empty span still plans one headless world, as
	// RenderGraph::Execute does. Width and height are the resolution of each
	// view; fixed-size resources continue to use their authored dimensions.
	ExecutionPlanStatus PlanFrame(
		const RenderGraph &graph,
		const ExecutionSchedule &schedule,
		std::span<const uint64_t> worlds,
		uint32_t width,
		uint32_t height,
		FrameExecutionPlan &out,
		core::Name &offender
	);

	// Which traffic-plan command buffer records a node's device work. CPU nodes
	// run on no device queue and belong to no buffer.
	enum class CommandBufferClass : uint8_t {
		Graphics,
		Compute,
		Transfer,
	};

	const char *Describe(CommandBufferClass bufferClass);

	// One command buffer of a schedule's traffic plan, in submission order.
	//
	// SDL exposes one unified queue rather than independent graphics, compute
	// and transfer queues, so these buffers cannot physically overlap -
	// submitting them in this order on that queue is what preserves every wave
	// dependency. The split is a structural boundary: dependency-bound compute
	// and later transfer work already sit in buffers of their own class, so a
	// backend with real device queues can lift each class onto its queue
	// without re-planning the frame.
	struct PlannedCommandBuffer {
		CommandBufferClass Class = CommandBufferClass::Graphics;

		// The schedule waves this buffer spans, inclusive. Buffers of different
		// classes share a wave when it holds independent work for several
		// queues.
		size_t FirstWave = 0;
		size_t LastWave = 0;

		// Every scheduled node recording into this buffer, in wave order.
		std::vector<NodeId> Nodes;
	};

	// Splits a compiled schedule into traffic-plan command buffers.
	//
	// Consecutive waves of one queue class share a buffer, because a boundary
	// with the same class on both sides orders nothing. Within one wave the
	// classes are emitted transfer, compute, graphics: wave members are
	// independent by definition, and that order matches how the backend already
	// submits uploads and async-eligible compute ahead of raster work.
	std::vector<PlannedCommandBuffer> PlanCommandBuffers(const ExecutionSchedule &schedule);
}
