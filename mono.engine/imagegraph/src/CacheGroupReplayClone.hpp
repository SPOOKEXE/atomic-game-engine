#pragma once

#include <engine/imagegraph/CacheGroupReplay.hpp>

namespace engine::imagegraph::detail {
	Status InitializeGroupRenderOutputs(
		const Document &document,
		const CacheGroupReplayState &source,
		CacheGroupReplayState &output,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	);
	// Reconciliation admits value clone storage beside embedded output records.
	uint64_t CacheGroupReplayCloneBytes(const CacheGroupReplayState &state);
	uint64_t CacheGroupReplayComparisonWork(const CacheGroupReplayState &state);
	// grug synchronize native authored edits, separate from source deserialization refresh.
	Status SynchronizeAuthoredCacheGroupMetadata(
		const Document &document,
		std::span<const std::string_view> owners,
		std::span<const std::string_view> serializeOwners,
		std::span<CacheGroupReplayState *const> journals,
		uint64_t maximumBytes,
		Diagnostic &diagnostic,
		uint64_t &remainingWork
	);
}
