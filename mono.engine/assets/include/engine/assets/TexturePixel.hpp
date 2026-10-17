#pragma once

// Typed access to tightly packed TextureData pixels.
//
// @tier L8 · shared

#include <engine/assets/Texture.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace engine::assets {
	// Four numeric channels decoded from one texture pixel.
	using TexturePixel = std::array<double, 4>;

	// Reads one pixel without changing its numeric range. Single-channel formats
	// use R and leave G/B at zero and A at one.
	// @param format The stored pixel layout.
	// @param bytes Exactly one encoded pixel.
	// @param pixel Receives the decoded channels only on success.
	// @return Whether the format and byte span are valid.
	bool
	LoadTexturePixel(TextureFormat format, std::span<const std::byte> bytes, TexturePixel &pixel) noexcept;

	// Encodes one pixel. Floating formats retain signed and HDR values; normalized
	// formats clamp to [0,1]. Four-bit channels round to nearest, ties away from
	// zero.
	// @param format The destination pixel layout.
	// @param pixel The numeric channels.
	// @param bytes Exactly one destination pixel.
	// @return Whether the format, span and active channel values are valid.
	bool
	StoreTexturePixel(TextureFormat format, const TexturePixel &pixel, std::span<std::byte> bytes) noexcept;

	// Converts one pixel for an SDR display callback. Float samples clamp only
	// here; single-channel data is shown as grayscale with opaque alpha.
	// @param format The stored pixel layout.
	// @param bytes Exactly one encoded pixel.
	// @param rgba Receives finite [0,1] display channels only on success.
	// @return Whether the format and byte span are valid.
	bool LoadTexturePixelForDisplay(
		TextureFormat format, std::span<const std::byte> bytes, std::array<float, 4> &rgba
	) noexcept;
} // namespace engine::assets
