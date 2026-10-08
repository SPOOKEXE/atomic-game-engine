#pragma once

#include <engine/imagegraph/Document.hpp>

#include <nodegraph/Graph.hpp>
#include <span>

namespace studio {
	// Copy/paste duplicates widgets, so settle new document identities before recording an edit or output.
	void AssignImageComposerIdentities(
		nodegraph::Graph &graph, std::span<const engine::imagegraph::Output> outputs
	);
}
