#pragma once

// Headless character coverage from exact caller-provided font bytes.
// @tier L7 · shared

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::gui {
	constexpr size_t MAXIMUM_FONT_GLYPH_FILE_BYTES = 16 * 1024 * 1024;
	constexpr size_t MAXIMUM_FONT_GLYPH_CHARACTERS = 4096;
	constexpr uint16_t MAXIMUM_FONT_GLYPH_PIXEL_SIZE = 512;
	constexpr size_t MAXIMUM_FONT_GLYPH_PAYLOAD_BYTES = 4 * 1024 * 1024;
	constexpr size_t MAXIMUM_FONT_GLYPH_OPERATION_BYTES = 64 * 1024 * 1024;

	enum class FontGlyphStatus : uint8_t {
		Ok,
		InvalidRequest,
		InvalidFont,
		UnsupportedFont,
		UnsupportedCoverage,
		LimitExceeded,
		AllocationFailed,
		DecodeFailed,
	};

	struct FontGlyphRequest {
		std::span<const std::byte> FontBytes;
		// Unique Unicode scalars, retained in the requested order. No fallback characters are added.
		std::span<const uint32_t> Characters;
		uint16_t PixelSize = 16;
		bool Antialias = true;
		// Includes supplied font/request bytes, fixed validation scratch, vendor allocations and output.
		size_t MaximumOperationBytes = MAXIMUM_FONT_GLYPH_OPERATION_BYTES;
	};

	struct FontGlyphCoverage {
		uint32_t Character = 0;
		uint32_t GlyphIndex = 0;
		// Missing characters have index zero, zero metrics and no coverage; spaces may be present/empty.
		bool Present = false;
		double AdvanceXPixels = 0;
		double AdvanceYPixels = 0;
		// Top-left of coverage relative to the baseline, with positive Y downward.
		int32_t OffsetXPixels = 0;
		int32_t OffsetYPixels = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Top-to-bottom rows, one coverage byte per pixel, with no padding.
		std::vector<uint8_t> Coverage;
	};

	struct FontGlyphBatch {
		std::vector<FontGlyphCoverage> Glyphs;
		double AscentPixels = 0;
		double DescentPixels = 0;
		double LineHeightPixels = 0;
		// Owned vector capacities, excluding this fixed-size value and any prior caller-owned result.
		size_t RetainedBytes = 0;
		// Peak accounted operation storage, excluding allocator implementation overhead and call stacks.
		size_t PeakOperationBytes = 0;
	};

	// Opens face zero with its Unicode charmap. Scalable outlines and BDF/PCF mono/gray strikes are
	// supported. WOFF2 and embedded color/bitmap font codecs are refused to retain bounded allocation.
	// No shaping, kerning, fallback, SDF conversion or filesystem access. Failure preserves output.
	[[nodiscard]] FontGlyphStatus DecodeFontGlyphs(const FontGlyphRequest &request, FontGlyphBatch &output);
}
