#pragma once

// A recipe as an ANG graph, for inspecting and drawing it. The node types
// describe and link; the recipe itself runs through the region compiler.
// @tier L3 · shared

#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>
#include <engine/procedural/Canonical.hpp>

namespace engine::procedural {

	// Process-lifetime node types: one per authored operation kind, and
	// "procedural.recipe" for the values the whole recipe owns.
	const ang::Registry &RecipeGraphTypes();

	// Replaces `out` with the recipe's nodes, links and values. False when the
	// recipe repeats an id or names a missing node; `out` is then unchanged.
	bool ToAngGraph(const Recipe &recipe, ang::Graph &out);
}
