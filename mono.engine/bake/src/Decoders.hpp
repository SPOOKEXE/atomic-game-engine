#pragma once

// The per-format entry points `Image.cpp` dispatches to.
//
// Private, because which formats exist is `ImageFormat`'s business and a caller
// that reached for `ReadPng` directly would be choosing a decoder by name -
// which is exactly the mistake `ReadImage` sniffing the signature exists to
// prevent.

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace engine::bake {

	// Decodes a PNG into RGBA8.
	//
	// @param bytes   The file, signature included.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadPng(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Decodes a baseline JPEG into RGBA8.
	//
	// @param bytes   The file, the start-of-image marker included.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadJpeg(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Decodes a GIF into one flipbook sheet.
	//
	// **A grid, not a list of frames**, because a flipbook is the thing this
	// engine can already draw animated - `effects::FlipbookLayout`. `Gif.cpp`
	// carries the whole argument and what it costs.
	//
	// @param bytes   The file, `GIF8` included.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadGif(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Decodes a Windows BMP into RGBA8.
	//
	// @param bytes   The file, `BM` included.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadBmp(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Rasterises the subset of SVG `Svg.cpp` draws, at an explicit size.
	//
	// **The one entry here that takes a size, because an SVG has none.** Every
	// other format states its own dimensions and this one states a coordinate
	// system, so the pixels are the caller's decision - which is why the public
	// name for this is `RasterizeSvg` and why the pipeline reaches it through a
	// node that carries the target rather than through `ReadImage`.
	//
	// @param bytes   The document.
	// @param width   The target width, or zero with `height` for the size the
	//                document itself declares.
	// @param height  The target height, under the same rule.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, naming whatever was refused.
	// @return `false` on anything malformed, unsupported, or past a bound.
	// @since v0.14
	bool ReadSvg(
		std::span<const std::byte> bytes,
		uint32_t width,
		uint32_t height,
		assets::TextureData &out,
		std::string &failure
	);
}
