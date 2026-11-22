#pragma once

#include <engine/render/presentation/WorldPresentation.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Applies source-owned state before the list becomes the stable source-row prefix.
	void FinalizePresentationSourceRows(ecs::Store &store, DrawList &drawList, bool removeFullyTransparent);

	// Replaces derived source cuts and appends seam copies and physical
	// particles after the cached prefix. Returns whether any row may differ
	// from a list that carried no cut and no derived row.
	bool AppendPresentationSeamRows(ecs::Store &store, DrawList &drawList);
}
