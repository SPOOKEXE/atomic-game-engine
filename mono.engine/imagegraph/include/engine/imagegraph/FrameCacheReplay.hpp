#pragma once
#include <engine/imagegraph/DataReplay.hpp>

namespace engine::imagegraph {
	// Private replay tags identify the source node type without adding authored schema fields.
	std::string_view SourceFrameCacheRowType(const DataReplayEntry &entry);
	const Value *SourceFrameCacheLastOutput(const DataReplayEntry &entry);
	enum class FrameCacheOutputPolicy : uint8_t { RetainedObservation, PreObservation, Constructor };
	// Merge captured frame histories transactionally. PreObservation keeps the target's earlier
	// output, while Constructor resets only latest output for an explicit native played-prefix seek.
	Status OverlaySourceFrameCacheRows(
		const Document &document,
		const DataReplayState &retained,
		DataReplayState &target,
		FrameCacheOutputPolicy policy,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
