#pragma once

// arch-waiver public-header: forward studio API. Render-pipeline tooling uses
// this complete graph contract.

// The adapter between the engine's render-pipeline document and the generic
// nodegraph canvas. The canvas owns editing gestures; this owns what its typed
// sockets mean to a render graph.

#include <engine/graph/PipelineDocument.hpp>

#include <nodegraph/Graph.hpp>
#include <optional>
#include <string>

namespace engine::render {
	class Renderer;
}

namespace studio {

	// Registers render resource types and every catalogue pass with nodegraph.
	// Idempotent, so a panel and a headless test can both call it.
	void RegisterRenderPipelineNodeTypes();

	// Rebuilds a canvas graph from a saved pipeline. Pass output bindings are
	// retained as hidden node values, while reads become typed links.
	bool LoadRenderPipelineGraph(
		const engine::graph::PipelineDocument &document, nodegraph::Graph &graph, std::string &error
	);

	// Rebuilds an engine document from the canvas. Existing resource descriptors
	// are retained and newly introduced outputs receive descriptors from their
	// catalogue ports.
	bool SaveRenderPipelineGraph(
		const nodegraph::Graph &graph,
		const engine::graph::PipelineDocument &basis,
		engine::graph::PipelineDocument &document,
		std::string &error
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
