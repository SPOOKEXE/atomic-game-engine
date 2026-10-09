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
	// stores authored cells without atlas padding. The renderer packs them into
	// a bounded timed atlas when the content arrives.
	struct TextureSequenceData {
		// Pixel width shared by every frame.
		uint32_t Width = 0;
		// Pixel height shared by every frame.
		uint32_t Height = 0;
		// Pixel encoding shared by every frame.
		TextureFormat Format = TextureFormat::RGBA8;
		// Playback duration of each frame, in seconds.
		std::vector<float> FrameDurations;
		// Concatenated frame pixels in playback order.
		std::vector<std::byte> Pixels;

		// Checks dimensions, format, durations, and exact pixel storage size.
		bool IsValid() const;
		// Returns one frame's pixels, or an empty span for an invalid index.
		std::span<const std::byte> FramePixels(size_t index) const;
	};

	// Native `.aseq` payload. No renderer or filesystem dependency belongs here.
	class TextureSequence {
	  public:
		// File signature identifying the native ASQ1 sequence container.
		static constexpr uint32_t MAGIC = 0x31515341; // ASQ1
		// Current serialized container version.
		static constexpr uint16_t VERSION = 1;
		// Maximum accepted number of frames in one sequence.
		static constexpr uint32_t MAXIMUM_FRAMES = 4096;
		// Maximum decoded pixel payload accepted by the container.
		static constexpr uint64_t MAXIMUM_PIXEL_BYTES = 256ull * 1024 * 1024;

		// Appends one sequence container when data satisfies its format limits.
		static bool Write(core::ByteWriter &writer, const TextureSequenceData &data);
		// A malformed payload leaves `out` unchanged.
		static bool Read(core::ByteReader &reader, TextureSequenceData &out);
	};
}
