#pragma once
#include <engine/imagegraph/DataReplay.hpp>

namespace engine::imagegraph {
	// Native metadata records a freed source slot without inventing a device handle.
	bool IsFreedCacheResultsSlot(const ElementValue &slot);
	bool IsFreedCacheResultsSlot(const Value &slot);
	// Conservative candidate peak; source residency and validation are charged separately.
	uint64_t ClearedCacheResultsReplayBytes(const DataReplayState &source, std::string_view nodeId);
	// Preserves the source cycle count and observation clock, frees owned pixels and resets index zero.
	// maximumBytes includes source, prior output, replacement and validation workspace.
	Status ClearCacheResultsReplay(
		const DataReplayState &source,
		std::string_view nodeId,
		DataReplayState &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
