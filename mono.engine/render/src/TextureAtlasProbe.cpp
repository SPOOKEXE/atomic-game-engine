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

	TextureAtlasPlan PlanTextureAtlas(uint32_t sourceCount, uint32_t sourceExtent, uint32_t mipLevels) {
		TextureAtlasPlan plan;
		if (sourceCount == 0 || sourceExtent == 0 || mipLevels != 1) {
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

	TextureAtlasResidency::TextureAtlasResidency(TextureAtlasPlan plan)
		: Layout(std::move(plan)), Resident(Layout.Rects.size(), false) {}

	TextureAtlasRequest TextureAtlasResidency::Request(uint32_t source) {
		if (!Layout.Valid() || source >= Layout.Rects.size()) {
			return {};
		}
		if (Resident[source]) {
			Counts.Hits++;
			return {.Rect = Layout.Rects[source]};
		}
		Counts.Misses++;
		return {
			.Rect = Layout.Rects[source],
			.Page = 0,
			.AllocatePage = !PageResident,
			.Upload = true,
		};
	}

	void TextureAtlasResidency::PageAllocated(uint32_t page) {
		if (page == 0 && !PageResident) {
			PageResident = true;
			Counts.PageAllocations++;
		}
	}

	void TextureAtlasResidency::Copied(uint32_t source) {
		if (source < Resident.size() && !Resident[source]) {
			Resident[source] = true;
			Counts.CopyCalls++;
		}
	}
}
