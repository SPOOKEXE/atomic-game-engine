#pragma once

// The adapter between the engine's render-pipeline document and the generic
// nodegraph canvas. The canvas owns editing gestures; this owns what its typed
// sockets mean to a render graph.

#include <engine/graph/PipelineDocument.hpp>

#include <nodegraph/Graph.hpp>
#include <string>

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
}
