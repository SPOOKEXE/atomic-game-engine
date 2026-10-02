#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {
	struct GifFrame {
		uint32_t Width = 0, Height = 0;
		std::span<const uint8_t> Rgba;
		uint16_t DelayCentiseconds = 1;
	};
	// Native CPU profile: quality 0..3 retains 2/4/6/8 channel bits before deterministic
	// weighted palette reduction. Alpha below 128 is transparent, otherwise opaque.
	// At most 4096 equal-size frames and 16 million total pixels. The byte cap includes
	// encoder-owned peak workspace and output, excluding borrowed frames and the caller's
	// old output. Loop count zero repeats indefinitely. Failure preserves output.
	bool WriteGif(
		std::span<const GifFrame> frames,
		uint8_t quality,
		uint64_t maximumBytes,
		std::vector<std::byte> &output,
		std::string &failure,
		uint16_t loopCount = 0
	);
}
