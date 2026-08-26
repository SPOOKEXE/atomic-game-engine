#include "IndexResidency.hpp"

#include <engine/parallel/Jobs.hpp>

#include <algorithm>

namespace engine::render {

	void IndexResidency::Plan(uint32_t version, std::span<const uint32_t> indices, bool bufferReplaced) {
		PendingVersion = version % VERSIONS;
		Pending = indices;
		Ranges.clear();
		Dirty = 0;

		const VersionState &state = States[PendingVersion];
		if (bufferReplaced || !state.Initialised) {
			AddRange(0, static_cast<uint32_t>(indices.size()));
			return;
		}

		const size_t common = std::min(indices.size(), state.Indices.size());
		constexpr size_t COMPARE_GRAIN = 4096;
		constexpr size_t PARALLEL_MINIMUM = 16'384;
		if (common < PARALLEL_MINIMUM) {
			size_t first = common;
			for (size_t index = 0; index < common; index++) {
				if (indices[index] != state.Indices[index]) {
					if (first == common) {
						first = index;
					}
					continue;
				}
				if (first != common) {
					AddRange(static_cast<uint32_t>(first), static_cast<uint32_t>(index - first));
					first = common;
				}
			}
			if (first != common) {
				AddRange(static_cast<uint32_t>(first), static_cast<uint32_t>(common - first));
			}
		} else {
			const size_t chunkCount = (common + COMPARE_GRAIN - 1) / COMPARE_GRAIN;
			Chunks.resize(chunkCount);
			parallel::Jobs::For(
				chunkCount,
				1,
				[&](size_t firstChunk, size_t lastChunk) {
					for (size_t chunkIndex = firstChunk; chunkIndex < lastChunk; chunkIndex++) {
						const size_t begin = chunkIndex * COMPARE_GRAIN;
						const size_t end = std::min(begin + COMPARE_GRAIN, common);
						std::vector<InstanceUploadRange> &ranges = Chunks[chunkIndex].Ranges;
						ranges.clear();
						size_t first = end;
						for (size_t index = begin; index < end; index++) {
							if (indices[index] != state.Indices[index]) {
								if (first == end) {
									first = index;
								}
								continue;
							}
							if (first != end) {
								ranges.push_back({
									static_cast<uint32_t>(first),
									static_cast<uint32_t>(index - first),
								});
								first = end;
							}
						}
						if (first != end) {
							ranges.push_back({
								static_cast<uint32_t>(first),
								static_cast<uint32_t>(end - first),
							});
						}
					}
				},
				2
			);
			for (size_t chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
				for (const InstanceUploadRange &range : Chunks[chunkIndex].Ranges) {
					AddRange(range.First, range.Count);
				}
			}
		}
		if (indices.size() > common) {
			AddRange(static_cast<uint32_t>(common), static_cast<uint32_t>(indices.size() - common));
		}
	}

	void IndexResidency::Acknowledge() {
		VersionState &state = States[PendingVersion];
		if (state.Indices.size() < Pending.size()) {
			state.Indices.resize(Pending.size());
		}
		for (const InstanceUploadRange &range : Ranges) {
			std::copy_n(Pending.begin() + range.First, range.Count, state.Indices.begin() + range.First);
		}
		state.Indices.resize(Pending.size());
		state.Initialised = true;
		Ranges.clear();
		Dirty = 0;
		Pending = {};
	}

	void IndexResidency::AddRange(uint32_t first, uint32_t count) {
		if (count == 0) {
			return;
		}
		if (!Ranges.empty() && Ranges.back().First + Ranges.back().Count == first) {
			Ranges.back().Count += count;
		} else {
			Ranges.push_back({first, count});
		}
		Dirty += count;
	}
}
