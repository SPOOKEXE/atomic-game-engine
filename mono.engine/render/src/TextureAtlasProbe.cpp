#include "TextureAtlasProbe.hpp"

#include <algorithm>
#include <limits>

namespace engine::render {

	bool TextureAtlasPlan::Valid() const {
		if (PageWidth == 0 || PageHeight == 0 || Rects.empty()) {
			return false;
		}
		for (const TextureAtlasRect &rect : Rects) {
			if (rect.Width == 0 || rect.Height == 0 || rect.Width > PageWidth || rect.Height > PageHeight ||
				rect.X > PageWidth - rect.Width || rect.Y > PageHeight - rect.Height) {
				return false;
			}
		}
		return true;
	}

	TextureAtlasPlan PlanTextureAtlas(uint32_t sourceCount, uint32_t sourceExtent) {
		TextureAtlasPlan plan;
		if (sourceCount == 0 || sourceExtent == 0) {
			return plan;
		}

		uint32_t columns = 1;
		while (columns < sourceCount / columns + (sourceCount % columns != 0)) {
			columns++;
		}
		const uint32_t rows = sourceCount / columns + (sourceCount % columns != 0);
		if (columns > std::numeric_limits<uint32_t>::max() / sourceExtent ||
			rows > std::numeric_limits<uint32_t>::max() / sourceExtent) {
			return plan;
		}
		plan.PageWidth = columns * sourceExtent;
		plan.PageHeight = rows * sourceExtent;
		plan.Rects.reserve(sourceCount);
		for (uint32_t index = 0; index < sourceCount; index++) {
			plan.Rects.push_back({
				.X = index % columns * sourceExtent,
				.Y = index / columns * sourceExtent,
				.Width = sourceExtent,
				.Height = sourceExtent,
			});
		}
		return plan;
	}
}
