#pragma once

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::core {
	class ByteReader;
	class ByteWriter;
}

namespace engine::assets {
	// Frames are full, equal-size RGBA8 images in playback order. The sequence
	// remains a distinct asset because a 2D flipbook atlas cannot address more
	// than 256 cells in the current particle draw.
	struct TextureSequenceData {
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;
		std::vector<float> FrameDurations;
		std::vector<std::byte> Pixels;

		bool IsValid() const;
		std::span<const std::byte> FramePixels(size_t index) const;
	};

	// Native `.aseq` payload. No renderer or filesystem dependency belongs here.
	class TextureSequence {
	  public:
		static constexpr uint32_t MAGIC = 0x31515341; // ASQ1
		static constexpr uint16_t VERSION = 1;
		static constexpr uint32_t MAXIMUM_FRAMES = 4096;
		static constexpr uint64_t MAXIMUM_PIXEL_BYTES = 256ull * 1024 * 1024;

		static bool Write(core::ByteWriter &writer, const TextureSequenceData &data);
		// A malformed payload leaves `out` unchanged.
		static bool Read(core::ByteReader &reader, TextureSequenceData &out);
	};
}
