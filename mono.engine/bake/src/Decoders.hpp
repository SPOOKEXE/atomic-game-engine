#pragma once

// The per-format entry points `Image.cpp` dispatches to.
//
// Private, because which formats exist is `ImageFormat`'s business and a caller
// that reached for `ReadPng` directly would be choosing a decoder by name —
// which is exactly the mistake `ReadImage` sniffing the signature exists to
// prevent.

#include <engine/assets/Texture.hpp>

#include <cstddef>
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

	// Decodes a Windows BMP into RGBA8.
	//
	// @param bytes   The file, `BM` included.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadBmp(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);
}
