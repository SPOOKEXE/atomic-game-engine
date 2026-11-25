#pragma once

// Image graph documents as editable ANG graphs: one node type per image kind,
// its properties as params with widget hints. Studio's composer and scripts
// edit the same form. The node types describe and link; evaluation stays with
// the document evaluator.
// @tier L9 · shared

#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>
#include <engine/imagegraph/Document.hpp>

#include <span>
#include <string_view>

namespace engine::imagegraph {

	// Process-lifetime and read-only; nothing registers into a global table.
	const ang::Registry &GraphTypes();

	// A node's value for one property, or its definition default.
	ang::Value GraphParam(const ang::NodeInstance &node, std::string_view key);

	// Replaces `out` with the document's nodes and links. Refuses a document that
	// does not compile and leaves `out` unchanged.
	bool ToAngGraph(const Document &document, ang::Graph &out, Diagnostic &diagnostic);

	// Reads the graph back into a checked document with these outputs. Parameters,
	// bindings and storage are not part of the graph and are left empty.
	bool FromAngGraph(
		const ang::Graph &graph, std::span<const Output> outputs, Document &out, Diagnostic &diagnostic
	);

	// The operation one placed node describes.
	bool ReadGraphOperation(const ang::NodeInstance &node, Operation &out, Diagnostic &diagnostic);
}
