#pragma once

// The device-free view of render node backends.
//
// Backend acceptance is derived from graph::NodeCatalogue. It lives apart
// from RenderTypes.hpp so headless tests can compare the two registries without
// acquiring SDL device layouts.

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/GraphRunner.hpp>

#include <vector>

namespace engine::render {

	// One built-in backend declaration derived from NodeKindSpec.
	struct BackendNode {
		// The node kind handled by the renderer.
		core::Name Kind;

		// Its default execution scope.
		graph::NodeScope Scope;

		// Its default device queue.
		graph::ExecutionQueue Queue;
	};

	// Returns every built-in backend declared by NodeCatalogue.
	inline std::vector<BackendNode> BackendNodes() {
		graph::RegisterRenderNodeKinds();
		std::vector<BackendNode> nodes;
		for (const graph::NodeKindSpec &spec : graph::NodeCatalogue::All()) {
			if (spec.BuiltInBackend) {
				nodes.push_back(BackendNode{spec.Kind, spec.Scope, spec.Queue});
			}
		}
		return nodes;
	}

	// Builds a handler table for every built-in backend kind.
	inline NodeTable BackendTable(const NodeHandler &handler) {
		graph::RegisterRenderNodeKinds();
		NodeTable nodes;
		for (const graph::NodeKindSpec &spec : graph::NodeCatalogue::All()) {
			if (spec.BuiltInBackend) {
				nodes.Set(spec.Kind, handler);
			}
		}
		return nodes;
	}
}
