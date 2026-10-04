#pragma once

#include "nodes/Sampler.hpp"

namespace engine::imagegraph::detail {
	enum class FontTextBlend : uint8_t { AlphaMultiply, AlphaAdd };
	struct FontGlyphPlacement {
		uint32_t Character = 0;
		Vector2 Position{}, Scale{1, 1};
		double Rotation = 0, VerticalOrigin = 0;
		Colour Tint{255, 255, 255, 255};
	};
	struct FontTextRasterOptions {
		SamplerSettings Sampler;
		FontTextBlend Blend = FontTextBlend::AlphaMultiply;
		const Image *Texture = nullptr;
		bool DebugTexture = false;
		bool NativeBitmapTexture = false;
		bool DistanceAntialias = false;
	};
	struct FontGlyphRasterFootprint {
		uint32_t Left = 0, Top = 0, Right = 0, Bottom = 0;
		uint64_t Work = 0;
	};
	// Explicit native pixel-center coverage. Measuring precedes every admitted batch draw.
	Status MeasureFontGlyphRaster(
		const FontData &,
		const FontGlyphPlacement &,
		const FontTextRasterOptions &,
		uint32_t width,
		uint32_t height,
		FontGlyphRasterFootprint &,
		std::string &failure
	);
	// The target is a private admitted candidate. This helper allocates no image or font storage.
	Status DrawFontGlyphRaster(
		const FontData &,
		const FontGlyphPlacement &,
		const FontTextRasterOptions &,
		const FontGlyphRasterFootprint &,
		Image &target,
		std::string &failure
	);
}
