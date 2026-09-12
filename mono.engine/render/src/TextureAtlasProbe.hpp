#pragma once

// A renderer-private texture-atlas residency experiment.

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
	// benchmark experiment, not a texture-table default or content format.
	// Mipmapped layouts are refused because they need gutters and generated
	// border texels to prevent one source bleeding into its neighbour.
	TextureAtlasPlan PlanTextureAtlas(uint32_t sourceCount, uint32_t sourceExtent, uint32_t mipLevels = 1);

	struct TextureAtlasUsage {
		uint64_t PageAllocations = 0;
		uint64_t CopyCalls = 0;
		uint64_t Hits = 0;
		uint64_t Misses = 0;
	};

	struct TextureAtlasRequest {
		TextureAtlasRect Rect;
		uint32_t Page = 0;
		bool AllocatePage = false;
		bool Upload = false;

		bool Valid() const {
			return Rect.Width != 0 && Rect.Height != 0;
		}
	};

	// Residency changes only after the benchmark reports an SDL allocation or
	// copy complete. Counters therefore describe work that happened on-device.
	class TextureAtlasResidency {
	  public:
		explicit TextureAtlasResidency(TextureAtlasPlan plan);

		TextureAtlasRequest Request(uint32_t source);
		void PageAllocated(uint32_t page);
		void Copied(uint32_t source);
		const TextureAtlasPlan &Plan() const {
			return Layout;
		}
		const TextureAtlasUsage &Usage() const {
			return Counts;
		}

	  private:
		TextureAtlasPlan Layout;
		std::vector<bool> Resident;
		bool PageResident = false;
		TextureAtlasUsage Counts;
	};
}
