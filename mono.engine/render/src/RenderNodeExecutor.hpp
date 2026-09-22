#pragma once

// The renderer-owned support table and custom handler lifecycle boundary.
//
// The graph catalogue describes authoring. This table declares the renderer's
// executable support and stays device free for admission tests.

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/GraphRunner.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace engine::render {

	// One renderer-supported built-in node declaration.
	struct BackendNode {
		// The node kind handled by the renderer.
		core::Name Kind;

		// Its default execution scope.
		graph::NodeScope Scope;

		// Its default device queue.
		graph::ExecutionQueue Queue;
	};

	// The capability list is deliberately independent from NodeCatalogue. A
	// catalogue entry remains authorable until this list gains its executor.
	inline const auto &BuiltInBackendKinds() {
		static constexpr std::array<std::string_view, 69> kinds{
			"world",
			"shadow",
			"camera",
			"entities",
			"cull-frustum",
			"cull-distance",
			"filter-tag",
			"order-draw",
			"mesh-residency",
			"delta-upload",
			"select-lod",
			"last-frame",
			"mirror-capture",
			"surface-capture",
			"portal-capture",
			"portal-tonemap",
			"forward",
			"gbuffer",
			"depth-peel",
			"depth-linearise",
			"depth-validity",
			"camera-motion",
			"depth-compose",
			"ambient-response",
			"ambient-merge",
			"ambient-correct",
			"colour-compose",
			"spatial-overlay",
			"transparent-layer",
			"fxaa",
			"taa",
			"smaa-edges",
			"smaa-blend",
			"smaa-resolve",
			"hzb",
			"skybox-compute",
			"clouds-compute",
			"ssao",
			"deferred-lighting",
			"sky",
			"fog",
			"shader-lenses",
			"bloom",
			"tonemap",
			"eye-image",
			"portal-overlay",
			"mirror-overlay",
			"transparent",
			"exposure-grade",
			"hsv",
			"mix",
			"transform-crop",
			"blur",
			"pack-channels",
			"blit",
			"raster",
			"dispatch",
			"present",
			"viewer",
			"capture",
			"shadow-capture",
			"overlay",
			"interface",
			"output-image",
			"tessellate",
			"tessellated-draw",
			"global-illumination",
			"raytrace",
			"pathtrace",
		};
		return kinds;
	}

	// Returns every built-in node the renderer can execute, with the graph's
	// scope and queue metadata used only after support has been selected here.
	inline std::vector<BackendNode> BackendNodes() {
		graph::RegisterRenderNodeKinds();
		std::vector<BackendNode> nodes;
		nodes.reserve(BuiltInBackendKinds().size());
		for (const std::string_view kind : BuiltInBackendKinds()) {
			const core::Name name(kind);
			const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(name);
			if (spec != nullptr) nodes.push_back(BackendNode{name, spec->Scope, spec->Queue});
		}
		return nodes;
	}

	// Builds a handler table for every built-in backend kind.
	inline NodeTable BackendTable(const NodeHandler &handler) {
		NodeTable nodes;
		for (const BackendNode &node : BackendNodes())
			nodes.Set(node.Kind, handler);
		return nodes;
	}
}
