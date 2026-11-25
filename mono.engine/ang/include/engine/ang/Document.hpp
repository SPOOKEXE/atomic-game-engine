#pragma once

// A graph document as text, for saving and for sending between processes.
//
// One line per node, param, link and group, with every name quoted and every
// number spelled so it reads back exactly. Node order is kept because it
// decides run order between independent nodes. Blobs have no text form.
// @tier L3 · shared

#include <engine/ang/Graph.hpp>

#include <string>
#include <string_view>

namespace engine::ang {

	// Bounds a reader enforces before it builds anything.
	struct DocumentLimits {
		size_t MaximumBytes = 1024 * 1024;
		size_t MaximumNodes = 4096;
		size_t MaximumLinks = 16384;
	};

	// Writes the document. False when a param holds a blob; `error` names it.
	bool WriteGraph(const Graph &graph, std::string &out, std::string &error);

	// Replaces `graph` with the document. On refusal `graph` is unchanged and
	// `error` names the line. Unknown node types still load.
	bool
	ReadGraph(std::string_view text, Graph &graph, std::string &error, const DocumentLimits &limits = {});
}
