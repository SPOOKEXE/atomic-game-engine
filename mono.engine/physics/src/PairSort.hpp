#pragma once

// Stable full-width entity ordering keeps each pair attached to its collider sources.
#include <engine/physics/PhysicsWorld.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::physics {
	constexpr size_t PAIR_RADIX_BITS = 11;
	constexpr size_t PAIR_RADIX_BUCKETS = size_t{1} << PAIR_RADIX_BITS;
	constexpr size_t PAIR_RADIX_PASSES = (sizeof(uint64_t) * 8 + PAIR_RADIX_BITS - 1) / PAIR_RADIX_BITS;

	template <typename Key>
	void RadixSortPairMember(std::vector<SourcedPair> &pairs, std::vector<SourcedPair> &scratch, Key key) {
		if (pairs.size() < 2) return;

		// Count every digit in one read. Constant digits are stable identity passes,
		// including the upper generation bits, so their full-array moves are unnecessary.
		std::array<std::array<size_t, PAIR_RADIX_BUCKETS>, PAIR_RADIX_PASSES> counts{};
		uint64_t varyingBits = 0;
		const uint64_t first = key(pairs.front());
		for (const SourcedPair &pair : pairs) {
			const uint64_t member = key(pair);
			varyingBits |= member ^ first;
			counts[0][member & (PAIR_RADIX_BUCKETS - 1)]++;
			counts[1][(member >> (PAIR_RADIX_BITS * 1)) & (PAIR_RADIX_BUCKETS - 1)]++;
			counts[2][(member >> (PAIR_RADIX_BITS * 2)) & (PAIR_RADIX_BUCKETS - 1)]++;
			counts[3][(member >> (PAIR_RADIX_BITS * 3)) & (PAIR_RADIX_BUCKETS - 1)]++;
			counts[4][(member >> (PAIR_RADIX_BITS * 4)) & (PAIR_RADIX_BUCKETS - 1)]++;
			counts[5][(member >> (PAIR_RADIX_BITS * 5)) & (PAIR_RADIX_BUCKETS - 1)]++;
		}

		scratch.resize(pairs.size());
		for (size_t pass = 0; pass < PAIR_RADIX_PASSES; pass++) {
			const size_t shift = pass * PAIR_RADIX_BITS;
			if (((varyingBits >> shift) & (PAIR_RADIX_BUCKETS - 1)) == 0) continue;
			auto &offsets = counts[pass];
			size_t next = 0;
			for (size_t &offset : offsets) {
				const size_t count = offset;
				offset = next;
				next += count;
			}
			for (const SourcedPair &pair : pairs) {
				const size_t bucket = static_cast<size_t>((key(pair) >> shift) & (PAIR_RADIX_BUCKETS - 1));
				scratch[offsets[bucket]++] = pair;
			}
			pairs.swap(scratch);
		}
	}
}
