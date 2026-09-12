#pragma once

// A renderer-private 4k texture packing experiment.
//
// The plan deliberately owns neither content names nor renderer residency. It
// only gives the benchmark a deterministic page and rectangles, leaving any
// future content-facing atlas policy at the renderer boundary.

#include <cstdint>
#include <vector>

namespace engine::render {

	struct TextureAtlasRect {
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	struct TextureAtlasPlan {
		uint32_t PageWidth = 0;
		uint32_t PageHeight = 0;
		std::vector<TextureAtlasRect> Rects;

		bool Valid() const;
	};

	// Packs equal square source textures into one smallest square grid page.
	//
	// Four 4096-pixel sources therefore occupy one 8192-pixel page. It is a
	// benchmark prototype, not a texture-table default or content format.
	TextureAtlasPlan PlanTextureAtlas(uint32_t sourceCount, uint32_t sourceExtent);
}
