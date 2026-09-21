#pragma once

#include <engine/render/WorldPresentation.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Applies source-owned state before the list becomes the stable source-row prefix.
	void FinalizePresentationSourceRows(ecs::Store &store, DrawList &drawList, bool removeFullyTransparent);

	// Appends view-derived seam copies after the cached source-row prefix.
	void AppendPresentationSeamRows(ecs::Store &store, DrawList &drawList);
}
