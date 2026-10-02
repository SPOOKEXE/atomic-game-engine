#pragma once

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {
	enum class LayeredImageFormat : uint8_t { OpenRaster, Krita };
	struct LayeredImageLayer {
		std::string Name;
		std::string Source;
		int32_t X = 0, Y = 0;
		assets::TextureData Pixels;
	};
	struct LayeredImage {
		LayeredImageFormat Format = LayeredImageFormat::OpenRaster;
		uint32_t Width = 0, Height = 0;
		assets::TextureData Merged;
		std::string Metadata;
		std::vector<LayeredImageLayer> Layers;
	};
	// Reads in-memory ZIP art archives. ORA layers preserve source offsets and names.
	// All member counts, compression sizes, checksums and image dimensions are checked before decoding.
	bool ReadLayeredImage(
		std::span<const std::byte> bytes,
		LayeredImageFormat format,
		LayeredImage &out,
		std::string &failure,
		uint64_t maximumDecodedBytes = 128ull * 1024 * 1024
	);
}
