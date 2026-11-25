#pragma once

// A render pipeline document as an ANG graph, for inspecting and drawing it
// outside the pipeline editor. Read-only: the document stays the source.
// @tier L9 · shared

#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace engine::graph {

	// Replaces `types` and `out` with the document's passes. Each pass is a node
	// of its kind; a read links to the last earlier enabled pass that wrote the
	// same resource. A disabled pass carries the param `enabled` = false. On refusal both are unchanged and
	// `error` says why.
	bool
	ToAngGraph(const PipelineDocument &document, ang::Registry &types, ang::Graph &out, std::string &error);

	// The names a set answers for, including the built-in "Default PBR".
	std::vector<std::string> PipelineGraphNames(const PipelineSet &set);

	// `ToAngGraph` for one named pipeline of a set or the built-in default.
	bool OpenPipelineGraph(
		const PipelineSet &set,
		std::string_view name,
		ang::Registry &types,
		ang::Graph &out,
		std::string &error
	);
}
