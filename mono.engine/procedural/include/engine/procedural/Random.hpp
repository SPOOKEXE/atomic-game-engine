#pragma once

// Version-one named counter streams. The algorithm and byte order are durable.
// @tier L3 · shared

#include <cstdint>
#include <string_view>

namespace engine::procedural {

	// FNV-1a over the stream's UTF-8 bytes, then four SplitMix64 rounds over
	// seed, signed global cell coordinates and counter. No mutable stream exists.
	// Labels such as position, rotation, variant and material isolate choices.
	uint64_t CounterWord(
		uint64_t rootSeed,
		std::string_view stream,
		int64_t globalCellX,
		int64_t globalCellY,
		uint64_t counter = 0
	);
}
