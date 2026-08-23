#pragma once

// Deterministic filtering for camera-owned entity whitelists.
//
// Chunks are independent while predicates run, then concatenated in source
// order. The result is byte-identical whether the job pool accepts the batch or
// runs it inline, which keeps draw order and replay stable.

#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph::detail {

	constexpr size_t FILTER_GRAIN = 4096;
	// Release measurements on twenty-four logical processors put 20,000 rows at
	// 344 us inline and 247 us pooled, and 100,000 at 1.76 ms and 451 us. Below
	// this floor the serial path retains its input-sized allocation and one walk.
	constexpr size_t FILTER_PARALLEL_MINIMUM = 16'384;

	template <typename IndexAt, typename Predicate>
	size_t StableFilter(size_t count, IndexAt indexAt, Predicate keep, std::vector<uint32_t> &into) {
		into.clear();
		into.reserve(count);

		if (count < FILTER_PARALLEL_MINIMUM) {
			for (size_t at = 0; at < count; at++) {
				const uint32_t index = indexAt(at);
				if (keep(index)) {
					into.push_back(index);
				}
			}
			return into.size();
		}

		struct Chunk {
			std::vector<uint32_t> Kept;
		};
		static thread_local std::vector<Chunk> chunks;
		std::vector<Chunk> &scratch = chunks;
		const size_t chunkCount = (count + FILTER_GRAIN - 1) / FILTER_GRAIN;
		scratch.resize(chunkCount);

		parallel::Jobs::For(
			chunkCount,
			1,
			[&](size_t firstChunk, size_t lastChunk) {
				for (size_t chunkIndex = firstChunk; chunkIndex < lastChunk; chunkIndex++) {
					const size_t begin = chunkIndex * FILTER_GRAIN;
					const size_t end = std::min(begin + FILTER_GRAIN, count);
					std::vector<uint32_t> &kept = scratch[chunkIndex].Kept;
					kept.clear();
					kept.reserve(end - begin);
					for (size_t at = begin; at < end; at++) {
						const uint32_t index = indexAt(at);
						if (keep(index)) {
							kept.push_back(index);
						}
					}
				}
			},
			2
		);

		for (size_t chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
			const std::vector<uint32_t> &kept = scratch[chunkIndex].Kept;
			into.insert(into.end(), kept.begin(), kept.end());
		}
		return into.size();
	}
}
