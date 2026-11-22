#pragma once

#include <engine/core/types/CFrame.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// True when two poses are equal component by component. Rows whose pose did
	// not change between ticks skip interpolation.
	inline bool SamePresentedFrame(const core::CFrame &left, const core::CFrame &right) {
		return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
			   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
			   left.QuaternionW == right.QuaternionW;
	}

	// Refreshes the cached pose columns without rebuilding static row data.
	size_t RefreshPresentationSourceRows(ecs::Store &store, DrawList &drawList, float alpha, size_t grain);
}
