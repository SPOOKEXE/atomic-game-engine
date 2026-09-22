#pragma once

// A deterministic, intentionally small software consumer of a DrawList.
//
// This is a reference for visual contracts, not a production painter.  It
// consumes resolved commands and caller supplied image bytes, so it remains
// usable in a headless process and never discovers a platform font or device.

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/ShapedText.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::gui {
	// Golden images are deliberately small. These bounds cover the returned
	// pixels and every isolated group held while the reference painter recurses.
	inline constexpr size_t MAXIMUM_REFERENCE_RASTER_PIXELS = 1'048'576;
	inline constexpr size_t MAXIMUM_REFERENCE_RASTER_BYTES = 64 * 1024 * 1024;
	inline constexpr size_t MAXIMUM_REFERENCE_RASTER_GROUPS = 8;
	inline constexpr size_t MAXIMUM_REFERENCE_RASTER_MASKS = 8;
	inline constexpr size_t MAXIMUM_REFERENCE_RASTER_GLYPHS = 4'096;
	inline constexpr size_t MAXIMUM_REFERENCE_GLYPH_CACHE_BYTES = 16 * 1024 * 1024;
	inline constexpr uint16_t MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE = 512;

	struct ReferencePixel {
		uint8_t R = 0;
		uint8_t G = 0;
		uint8_t B = 0;
		uint8_t A = 0;

		constexpr bool operator==(const ReferencePixel &) const = default;
	};

	struct ReferenceImage {
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<ReferencePixel> Pixels;

		bool Valid() const;
		const ReferencePixel &At(uint32_t x, uint32_t y) const;
	};

	struct ReferenceAsset {
		core::Name Name;
		ReferenceImage Image;
	};

	// Paints every DrawList primitive supported by a deterministic visual test.
	// When `fonts` is supplied, text consumes its canonical ShapedText positions
	// and rasterizes their pinned FreeType glyph coverage. It does not shape or
	// discover platform fonts. Missing or malformed glyph identities are skipped,
	// matching the renderer's local atlas-miss behaviour.
	ReferenceImage RasterizeReference(
		const DrawList &list,
		uint32_t width,
		uint32_t height,
		std::span<const ReferenceAsset> assets = {},
		const FontPackage *fonts = nullptr
	);

	// A stable content fingerprint for an accepted golden image. The test keeps
	// the expected value in source, making a visual change reviewable in a diff.
	uint64_t ReferenceImageHash(const ReferenceImage &image);

	struct ReferenceComparison {
		size_t ChangedPixels = 0;
		float ChangedArea = 0.0f;
		uint8_t MaximumChannelDifference = 0;
		bool WithinChangedAreaCap = true;
	};

	// Compares packed, deterministic pixels with a per-channel tolerance. A
	// caller supplies the maximum changed area as the visual contract, keeping a
	// broad change from passing because each individual pixel moved by one byte.
	ReferenceComparison CompareReferenceImages(
		const ReferenceImage &expected,
		const ReferenceImage &actual,
		uint8_t channelTolerance,
		float maximumChangedArea
	);
}
