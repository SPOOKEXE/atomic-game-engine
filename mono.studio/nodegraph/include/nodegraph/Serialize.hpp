#pragma once

// Saving a graph, and reading one back.
//
// **A line-based text format and not JSON**, which is the boring option on
// purpose: a graph is a handful of flat lists, and a format with no nesting
// needs no parser library and no vendored dependency in a template somebody is
// meant to copy. It diffs, too, which a single-line JSON blob does not.
//
//     nodegraph 1
//     node | 1 | math.add | 120 | 64 | my adder
//     value | 1 | bias | number | 0.5
//     link | 1 | Out | 2 | A
//
// Fields are pipe-separated because a label is whatever somebody typed and a
// comma is not rare in one.

#include <nodegraph/Graph.hpp>
#include <string>
#include <string_view>

namespace nodegraph {

	// Writes a graph as lines. **Not JSON**: a graph is three flat lists, and a
	// format with no nesting needs no parser and diffs line by line.
	std::string Save(const Graph &graph);

	// The same, for one node and everything inside it.
	//
	// **A whole document and not a fragment**, so filing a fold as a library
	// type and saving a graph produce the same thing, and so a template can be
	// opened, read and hand-edited like anything else here.
	std::string SaveSubtree(const Graph &graph, NodeId root);

	// Reads what `Save` wrote, replacing whatever `graph` held.
	//
	// A node of an unregistered type is kept; a link that cannot be made is
	// dropped. Refusing the whole file for one bad line would make a
	// hand-edited graph unopenable, which is the opposite of what a text format
	// is for.
	bool Load(std::string_view text, Graph &graph, std::string &error);
}
