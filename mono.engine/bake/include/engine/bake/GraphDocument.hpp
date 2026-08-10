#pragma once

// Where a document meets a graph.
//
// **The one function that needs both halves, which is why it is the only thing
// left here.** `bakegraph/Document.hpp` is the format — an ordered list of edits
// that saves, scripts and undoes — and it names no decoder, so `Engine::game`
// can read one out of a place file. Running a document needs `bake::Graph`,
// which needs every importer, so replaying one lives on this side of the line.
//
// `D00102` is the argument in full. Included from here as well, so a caller that
// wants to build a graph from a document writes one include and not three.
//
// @tier L9 · shared

#include <engine/bake/Graph.hpp>
#include <engine/bakegraph/Document.hpp>

#include <string>

namespace engine::bake {

	// Replays a document into a graph.
	//
	// **Into an empty graph.** Replaying onto one that already holds nodes would
	// make `Operation::From` mean a different node than it did when it was
	// recorded, which is the portability this file's position-based wiring
	// exists to have.
	//
	// @param document The edits.
	// @param graph    Filled in. Must be empty.
	// @param sources  Where a source's bytes come from. May be null when the
	//                 document holds no `AddSource`.
	// @param offender Set to the name or position of what failed. Untouched on
	//                 success.
	// @return `Ok`, or why not. The graph is left partly built on failure, for
	//         the reason a compiler still shows you the parse tree: an editor
	//         reporting "operation 7 was refused" is more use beside the six
	//         that worked.
	DocumentStatus
	Build(const Document &document, Graph &graph, const SourceResolver &sources, std::string &offender);
}
