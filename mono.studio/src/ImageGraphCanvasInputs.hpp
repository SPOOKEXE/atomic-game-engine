#pragma once

#include <engine/imagegraph/Catalogue.hpp>

#include <studio/ImageGraph.hpp>

namespace studio::detail {
	// Source nodes author named groups through their inspector. Generic native
	// array nodes start with one image socket.
	inline void SeedImageGraphCanvasInputs(nodegraph::Graph &graph, const ImageGraphCanvasIds &ids) {
		for (const auto &node : graph.Nodes()) {
			if (ids.ToDocument.contains(node.Id) || !node.DynamicInputs.empty() ||
				engine::imagegraph::FindCatalogueEntry(node.Type))
				continue;
			const auto *schema = engine::imagegraph::FindSchema(node.Type);
			if (schema && schema->DynamicInputs)
				(void)graph.SetDynamicInputs(node.Id, {{"item-1", "imagegraph.image"}});
		}
	}
}
