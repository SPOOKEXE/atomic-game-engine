#pragma once

#include <engine/render/WorldPresentation.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Refreshes the cached pose columns without rebuilding static row data.
	size_t RefreshPresentationSourceRows(ecs::Store &store, DrawList &drawList, float alpha, size_t grain);
}
