#pragma once

// Headless character coverage or signed distance from exact caller-provided font bytes.
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

	enum class FontGlyphRaster : uint8_t { Coverage, SignedDistance };

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
		FontGlyphRaster Raster = FontGlyphRaster::Coverage;
		// Native distance profile: spread2..32 pixels. Antialias applies only to Coverage.
		uint8_t DistanceSpread = 8;
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
		// Packed top-down bytes: coverage, or raw FreeType distance (128 contour, positive inside).
		// Distance in pixels is (byte - 128) * batch.DistanceSpread / 128.
		std::vector<uint8_t> Coverage;
		// Real distance-renderer border; zero for coverage and empty/missing glyphs.
		uint8_t DistancePaddingPixels = 0;
	};

	struct FontGlyphBatch {
		std::vector<FontGlyphCoverage> Glyphs;
		double AscentPixels = 0;
		double DescentPixels = 0;
		double LineHeightPixels = 0;
		// Owned vector capacities, excluding this fixed-size value and any prior caller-owned result.
		size_t RetainedBytes = 0;
		// Peak charged operation storage, including prior SDF output, excluding allocator overhead/stacks.
		size_t PeakOperationBytes = 0;
		FontGlyphRaster Raster = FontGlyphRaster::Coverage;
		uint8_t DistanceSpread = 0;
	};

	// Opens face zero with its Unicode charmap. Scalable outlines and BDF/PCF mono/gray strikes are
	// supported. WOFF2 and embedded color/bitmap font codecs are refused to retain bounded allocation.
	// SignedDistance uses real sdf/bsdf rendering, with unhinted scalable outlines. No shaping,
	// kerning, fallback or filesystem access. SDF charges prior output residency; Coverage retains
	// its existing independent-result ceiling. Failure preserves output.
	[[nodiscard]] FontGlyphStatus DecodeFontGlyphs(const FontGlyphRequest &request, FontGlyphBatch &output);
}
