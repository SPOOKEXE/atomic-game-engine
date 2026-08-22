#pragma once

// The node set the Demo Nodes panel runs, and a graph wired out of it.
//
// **Content, and not part of `Mono::nodegraph`, said out loud.** This is the
// file to copy: it registers terrain-shaped types through the same
// `NodeTypes::Register` anybody else would use, and nothing in the library
// knows it exists. It lives here rather than beside the library for that
// reason - a host with its own node vocabulary links `Mono::nodegraph` and
// gets none of this. The library's own suite registers its own fixture rather
// than reaching for these, so nothing but this panel depends on them.
//
// What it is *for* is that it exercises every part: both data types, every
// widget kind, a node with no evaluation, a node with two inputs, previews on a
// wire and on a type, and two genuinely slow async nodes so that two branches
// can be watched running at once.

#include <nodegraph/Graph.hpp>

namespace studio {

	// **Registered on first use rather than at static-initialisation time.** A
	// registry filled before `main` is a registry whose order depends on link
	// order, and the palette's order is the order things were registered in.
	void RegisterDemoNodes();

	// A wired graph to open the demo on, so the first thing anybody sees is a
	// graph rather than an empty grid.
	void BuildDemoGraph(nodegraph::Graph &graph);
}
