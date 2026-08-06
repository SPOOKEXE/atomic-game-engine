#pragma once

// Reading somebody else's image format, so the engine never has to.
//
// `assets::Texture` is the format this produces and its header carries the
// argument: **a runtime does not decode.** This is the other half of that
// sentence — the place the decoding actually happens, once, at publish time,
// in a program a player never runs.
//
// **The format is sniffed from the bytes and not from the name.** A file called
// `.png` that is a BMP is an ordinary accident in an art pipeline, and a
// decoder chosen by extension turns it into a corrupt-file error somebody has
// to trace back to the filename. The signature is two to eight bytes and it is
// never wrong.
//
// **Every length field in these formats is hostile**, and that is not a
// theoretical stance about art assets: this is the code a `cdn --publish` runs
// over a directory somebody uploaded. A chunk length is checked against what is
// actually present before anything is allocated, the inflated size is checked
// against what the header implies rather than being trusted from the stream,
// and a dimension past `assets::Texture::MAXIMUM_DIMENSION` is refused before
// the pixels are read.
//
// @tier L9 · shared

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::bake {

	// Which image format some bytes are.
	//
	// @since v0.9
	enum class ImageFormat : uint8_t {
		// Not a format this reads. The honest answer for a TGA, a DDS or a
		// text file, and not an error until somebody asks for its pixels.
		Unknown,

		// PNG, non-interlaced, 8 or 16 bits a channel.
		Png,

		// Windows BMP, uncompressed, 8, 24 or 32 bits a pixel.
		Bmp,

		// Baseline JPEG, greyscale or YCbCr.
		//
		// **Baseline and not progressive.** A progressive file arrives as
		// several passes over the same coefficients and is a different decoder;
		// one read as baseline produces a blurred version of the right image,
		// which is worse than a refusal because it looks like a quality setting.
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

	// Decodes an image into the engine's texture format.
	//
	// The output is always `RGBA8`: a decoder that preserved the source's
	// channel count would push the "how many channels is this" question
	// downstream to a renderer that has no way to ask. Widening one grey byte
	// into four costs the publisher a memcpy and costs a client nothing,
	// because zstd removes the three duplicated channels on the wire anyway.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, so a bake tool can name the file
	//                *and* the reason. Untouched on success.
	// @return `false` on anything this cannot read or will not trust.
	bool ReadImage(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Resamples an image with a box filter.
	//
	// **A box filter and not a nearest sample**, because a nearest downscale of
	// a character texture aliases into visible speckle the first time the model
	// is more than a few metres away — which reads as a mipmap bug in a
	// renderer that has no mipmaps yet.
	//
	// Upscaling is allowed and is a duplication rather than an interpolation:
	// there is no information to invent, and a bake step that blurred on the
	// way up would be lying about detail.
	//
	// @param source The image to resample.
	// @param width  The target width. Zero refuses.
	// @param height The target height. Zero refuses.
	// @param out    Filled on success. May not alias `source`.
	// @return `false` for a zero or over-large target, or an invalid source.
	bool
	ResizeImage(const assets::TextureData &source, uint32_t width, uint32_t height, assets::TextureData &out);
}
