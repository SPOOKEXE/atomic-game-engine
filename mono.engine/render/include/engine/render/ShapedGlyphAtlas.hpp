#pragma once

// Bounded coverage pages for glyph identities emitted by gui::ShapeText.
//
// This cache only rasterizes and packs. Placement stays the shaped glyph's X,
// Y and advances, so an atlas miss never becomes a second text measurement.
// A full page is evicted as one unit, which makes residency fixed and avoids a
// fragmented free-list that could grow without bound.

#include <engine/gui/ShapedText.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::render {

	struct ShapedAtlasGlyph {
		uint16_t Page = 0;
		uint16_t X = 0;
		uint16_t Y = 0;
		uint16_t Width = 0;
		uint16_t Height = 0;
		float OffsetX = 0.0f;
		float OffsetY = 0.0f;
		bool Present = false;
		bool operator==(const ShapedAtlasGlyph &) const = default;
	};

	class ShapedGlyphAtlas {
	  public:
		static constexpr uint16_t PAGE_EXTENT = 512;
		static constexpr uint16_t MAXIMUM_PAGES = 4;

		explicit ShapedGlyphAtlas(float pixelSize);

		// Resolves each shaped glyph in order. `FontPackage` owns the bytes used
		// for rasterization and must outlive this call.
		std::vector<ShapedAtlasGlyph> Resolve(
			const gui::FontPackage &package,
			std::span<const gui::ShapedGlyph> glyphs,
			uint64_t use,
			float pixelSize = 0.0f
		);

		uint16_t PageCount() const;
		const std::vector<uint8_t> &Coverage(uint16_t page) const;

		// Drops all coverage when the bytes behind a face identity change. A face
		// name alone cannot identify a rasterized glyph.
		void Clear();

	  private:
		struct Key {
			core::Name Face;
			uint32_t Index = 0;
			uint16_t PixelSize = 0;
			bool operator==(const Key &) const = default;
		};
		struct KeyHash {
			size_t operator()(const Key &key) const;
		};
		struct Entry {
			ShapedAtlasGlyph Glyph;
			uint16_t Page = 0;
		};
		struct Page {
			std::vector<uint8_t> Pixels;
			uint16_t NextX = 1;
			uint16_t NextY = 1;
			uint16_t RowHeight = 0;
			uint64_t LastUse = 0;
			std::vector<Key> Keys;
		};

		Page *AllocatePage(uint64_t use);
		float PixelSize;
		uint16_t PackingPage = UINT16_MAX;
		std::vector<Page> Pages;
		std::unordered_map<Key, Entry, KeyHash> Entries;
	};
}
