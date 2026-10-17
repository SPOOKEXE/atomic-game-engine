#pragma once

#include <engine/render/WorldPresentation.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Applies source-owned state before the list becomes the stable source-row prefix.
	void FinalizePresentationSourceRows(ecs::Store &store, DrawList &drawList, bool removeFullyTransparent);

	// Replaces derived source cuts and appends seam copies after the cached prefix.
	void AppendPresentationSeamRows(ecs::Store &store, DrawList &drawList);
}
