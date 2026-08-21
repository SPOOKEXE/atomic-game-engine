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
		// The authored node this came from, with its schedule already decided.
		ScheduledNode Scheduled;

		// What one invocation covers: a view, a world, or the whole frame. This
		// is what decides how many invocations one authored node expands into.
		NodeScope Scope = NodeScope::View;

		// Which view this invocation is for, or `RunContext::WHOLE_FRAME` when
		// the scope is not per-view.
		size_t View = RunContext::WHOLE_FRAME;

		// Which world, as a dense position numbered by first appearance in this
		// frame, or `RunContext::WHOLE_FRAME` when the scope is not per-world.
		//
		// **Dense rather than the caller's own number**, so a plan can index
		// arrays by it. `WorldKey` below is the value the caller recognises.
		size_t World = RunContext::WHOLE_FRAME;

		// The caller's stable identifier for that world, unchanged across
		// frames. `World` is this frame's position for it and is not.
		uint64_t WorldKey = 0;

		// What this invocation reads and writes, as resource bytes.
		//
		// Accounting rather than measurement: it is what the authored node
		// declared, summed, and it is the same figure on every backend because
		// nothing here has a device.
		//@{
		uint64_t ReadBytes = 0;
		uint64_t WriteBytes = 0;
		//@}
	};

	// Concrete invocations derived from one dependency wave.
	struct PlannedWave {
		// Everything in this wave, in no particular order - a wave is by
		// definition a set whose members do not depend on each other.
		std::vector<PlannedInvocation> Invocations;

		// Whether this wave's work lands on more than one device queue, so a
		// backend knows to expect the transfers `FrameExecutionPlan` lists.
		bool ConcurrentQueues = false;

		// Whether the invocations in this wave touch different worlds, and
		// different views, and may therefore run at the same time.
		//
		// **Two flags rather than one**, because a wave can be independent in
		// one axis and not the other: four views of one world are independent
		// views and a single world.
		//@{
		bool IndependentWorlds = false;
		bool IndependentViews = false;
		//@}
	};

	// A resource ownership handoff between device queues.
	//
	// Bytes describes the resource range made visible. It is synchronization
	// traffic, not necessarily a physical memory copy.
	struct QueueTransfer {
		// The resource whose ownership moves.
		ResourceId Resource;

		// The node that last wrote it, and the one about to read it.
		//@{
		NodeId Producer;
		NodeId Consumer;
		//@}

		// The queue it is leaving and the queue it is arriving on. Equal queues
		// are not recorded as a transfer at all.
		//@{
		ExecutionQueue From = ExecutionQueue::Graphics;
		ExecutionQueue To = ExecutionQueue::Graphics;
		//@}

		// Which view and world the handoff belongs to, or
		// `RunContext::WHOLE_FRAME` where it is not scoped to one.
		//@{
		size_t View = RunContext::WHOLE_FRAME;
		size_t World = RunContext::WHOLE_FRAME;
		//@}

		// How much of the resource is made visible. See the note above: this is
		// the range a barrier covers and not necessarily a copy.
		uint64_t Bytes = 0;
	};

	// Everything one frame will do, and what it will move to do it.
	//
	// **The whole point of it being device-free.** Studio's graph panel and every
	// renderer backend read this same structure, so the accounting a person is
	// shown is the accounting the backend acts on rather than a second estimate
	// that agrees until it does not.
	struct FrameExecutionPlan {
		// The waves, in the order they must run.
		std::vector<PlannedWave> Waves;

		// Every queue handoff the waves imply, across the whole frame.
		std::vector<QueueTransfer> Transfers;

		// The frame's totals, summed from every invocation above.
		//@{
		uint64_t ReadBytes = 0;
		uint64_t WriteBytes = 0;
		//@}

		// The bytes covered by `Transfers`, kept apart from the read and write
		// totals because synchronisation traffic is a different cost from work.
		uint64_t QueueTransferBytes = 0;

		// Empties the plan so one instance can be reused frame after frame,
		// which is what keeps planning out of the allocator.
		void Clear() {
			Waves.clear();
			Transfers.clear();
			ReadBytes = 0;
			WriteBytes = 0;
			QueueTransferBytes = 0;
		}
	};

	// Why a frame could not be planned.
	//
	// **Named rather than a bool**, because every one of these is a mistake in
	// an authored graph and the person who has to fix it is reading the message.
	enum class ExecutionPlanStatus : uint8_t {
		// The plan was produced.
		Ok,

		// A view was zero-sized, so no per-view resource has a size.
		InvalidDimensions,

		// The schedule named a node the graph does not have.
		MissingNode,

		// A node read or wrote a resource the graph does not have.
		MissingResource,

		// A node reached outside its own scope - a view node touching another
		// view's resource, or a frame node touching a per-world one. Nothing
		// downstream could give that a coherent meaning, so it is refused here
		// rather than resolved to whichever one happened to be first.
		InvalidScopeAccess,
	};

	// One line naming a status, for a message a person reads.
	//
	// @param status What went wrong.
	// @return Its name. Never null, and never empty.
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
		// Anything that draws.
		Graphics,

		// Dispatch work with no attachments.
		Compute,

		// Copies and uploads.
		Transfer,
	};

	// One line naming a buffer class, for a message a person reads.
	//
	// @param bufferClass The class.
	// @return Its name. Never null, and never empty.
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
		// Which queue this buffer would belong to on a backend that has more
		// than one. See the note above for why it is a boundary rather than a
		// promise of concurrency.
		CommandBufferClass Class = CommandBufferClass::Graphics;

		// The schedule waves this buffer spans, inclusive. Buffers of different
		// classes share a wave when it holds independent work for several
		// queues.
		//@{
		size_t FirstWave = 0;
		size_t LastWave = 0;
		//@}

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
