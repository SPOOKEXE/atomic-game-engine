#pragma once

// Lossless render admission into the engine executor. Resource versions,
// history, aliases and physical barriers remain owned by the render domain.
//
// @tier L9 shared

#include <engine/graph/EngineGraph.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace engine::graph {
	// Scope blocks retain their native frame/world/view invocation boundaries.
	enum class RenderStageBlock : uint8_t { Shared, PerView, Final };

	// Cached owner-thread invocation plan, with original render identity mapping.
	struct RenderStageProjection {
		// Named nodes/resources for inspection, never recompiled to infer versions.
		EngineGraph Graph;
		// Authoritative render waves filtered into Shared, PerView and Final blocks.
		std::array<EngineSchedule, 3> Blocks;
		// Dense EngineNodeId slot minus one to original render NodeId.
		std::vector<NodeId> RenderNodes;
	};

	// Preserves the public authored-order contract: serial owner-CPU invocation
	// waves copy the supplied block lists exactly. Refusal leaves out unchanged.
	EngineGraphStatus ProjectRenderStage(
		const RenderGraph &graph,
		const CompiledGraph &compiled,
		RenderStageProjection &out,
		core::Name &offender,
		std::string_view prefix = "render."
	);

	// Copies a validated domain schedule's exact wave/node order and queue policy
	// into the scope blocks. Never infers dependencies from unversioned names.
	// Refusal, including allocation failure, leaves out unchanged.
	EngineGraphStatus ProjectRenderStage(
		const RenderGraph &graph,
		const CompiledGraph &compiled,
		const ExecutionSchedule &schedule,
		RenderStageProjection &out,
		core::Name &offender,
		std::string_view prefix = "render."
	);

	// Invokes one view through the engine executor. Frame setup requires shared;
	// frame-scoped setup is skipped for later worlds while world setup still runs.
	bool ExecuteRenderView(
		const RenderGraph &graph,
		const RenderStageProjection &projection,
		NodeRunner &runner,
		size_t view,
		size_t world,
		bool shared,
		bool frame
	);
	// Invokes final work once, with no world or view identity.
	bool
	ExecuteRenderFinal(const RenderGraph &graph, const RenderStageProjection &projection, NodeRunner &runner);
	// Groups worlds in first-appearance order and executes each world's views
	// adjacently, then final work. Empty frames still execute shared setup once.
	bool ExecuteRenderStage(
		const RenderGraph &graph,
		const RenderStageProjection &projection,
		NodeRunner &runner,
		std::span<const uint64_t> worlds
	);
	// Executes a view count belonging to one world through the same executor.
	bool ExecuteRenderStage(
		const RenderGraph &graph, const RenderStageProjection &projection, NodeRunner &runner, size_t views
	);
}
