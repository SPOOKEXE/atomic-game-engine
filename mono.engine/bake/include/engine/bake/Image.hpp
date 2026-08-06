#pragma once

// Image import to `assets::TextureData`. Format detection uses bytes, and input
// lengths are checked before allocation.
// @tier L9 · shared

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::bake {

	// Image format.
	enum class ImageFormat : uint8_t {
		// Unsupported or unknown format.
		Unknown,

		// PNG, non-interlaced, 8 or 16 bits a channel.
		Png,

		// Windows BMP, uncompressed, 8, 24 or 32 bits a pixel.
		Bmp,

		// Baseline JPEG, greyscale or YCbCr.
		//
		// Baseline JPEG only.
		Jpeg,
	};

	// Identifies a format from the leading bytes.
	//
	// @param bytes The file, or as much of its front as is available.
	// @return The format, or `Unknown`.
	ImageFormat ImageFormatOfBytes(std::span<const std::byte> bytes);

	// A name for a format, for a log line and a command line.
	//
	// @param format The format.
	// @return A view valid for the lifetime of the process.
	std::string_view Describe(ImageFormat format);

	// Decodes an image into RGBA8 texture data.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, so a bake tool can name the file
	//                *and* the reason. Untouched on success.
	// @return `false` on anything this cannot read or will not trust.
	bool ReadImage(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Resamples an image with a box filter. Upscaling duplicates source pixels.
	//
	// @param source The image to resample.
	// @param width  The target width. Zero refuses.
	// @param height The target height. Zero refuses.
	// @param out    Filled on success. May not alias `source`.
	// @return `false` for a zero or over-large target, or an invalid source.
	bool
	ResizeImage(const assets::TextureData &source, uint32_t width, uint32_t height, assets::TextureData &out);
}
