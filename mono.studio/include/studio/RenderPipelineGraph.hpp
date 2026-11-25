#pragma once

// arch-waiver public-header: forward studio API. Render-pipeline tooling uses
// this complete graph contract.

// The adapter between the engine's render-pipeline document and an ang graph
// drawn by an angview canvas. The canvas owns editing gestures; this owns what
// its typed ports mean to a render graph.
//
// Node ids are durable document ids: a loaded pass keeps its pass name as its
// id, and renaming a pass changes its label param, never its id.

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/render/Renderer.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace studio {

	// Param holding a pass's name. Absent on a new pass, which saves as its kind.
	inline constexpr const char *RENDER_PIPELINE_LABEL = "__render.label";

	// Render resource types and one definition per catalogue pass, built once.
	const engine::ang::Registry &RenderPipelineTypes();

	// A node's param, or its definition default, or an empty value.
	engine::ang::Value RenderPipelineParam(const engine::ang::NodeInstance &node, std::string_view key);

	// The pass name a node shows and saves under: its label, else its kind.
	std::string RenderPipelineLabel(const engine::ang::NodeInstance &node);

	// The image output a pass previews, or empty when it writes no image.
	std::string RenderPipelinePreviewPort(std::string_view def);

	// Rebuilds a canvas graph from a saved pipeline. Pass output bindings are
	// retained as hidden params, while reads become typed links.
	bool LoadRenderPipelineGraph(
		const engine::graph::PipelineDocument &document, engine::ang::Graph &graph, std::string &error
	);

	// Rebuilds an engine document from the canvas. Existing resource descriptors
	// are retained and newly introduced outputs receive descriptors from their
	// catalogue ports. Nodes save in graph order, so canvas node `i` is render
	// node `i + 1`.
	bool SaveRenderPipelineGraph(
		const engine::ang::Graph &graph,
		const engine::graph::PipelineDocument &basis,
		engine::graph::PipelineDocument &document,
		std::string &error
	);

	// Canvas run feedback: per node, its status and CPU time from the renderer's
	// observed frame, the GPU time from `profile` where one is measured, and the
	// bytes of the resources it writes. A node the frame did not visit reports
	// Idle so the canvas shows its bytes alone. `timeline` gets the observed
	// frame. `built` is the render graph built from the saved document.
	void RenderPipelineFeedback(
		const engine::ang::Graph &graph,
		const engine::graph::RenderGraph &built,
		const engine::graph::PipelineProfile &profile,
		const engine::render::Renderer::PipelineRun &run,
		engine::ang::Plan &plan,
		engine::ang::RunReport &report,
		engine::ang::RunTimeline &timeline
	);

	// Whether the render pipeline panels should load for `activeWorld`. Only a
	// change of world loads: a failed or empty load is reported once and not
	// retried every frame, because each load resets selection and view and so
	// killed every drag in the canvas. Explicit profile picks load directly.
	inline bool RenderPipelineNeedsLoad(std::optional<uint32_t> triedWorld, uint32_t activeWorld) {
		return !triedWorld.has_value() || *triedWorld != activeWorld;
	}

	// Reports the renderer's admission failure without changing the document.
	bool ValidateRenderPipelineAdmission(
		const engine::graph::PipelineDocument &document,
		const engine::render::Renderer &renderer,
		std::string &error
	);
}
