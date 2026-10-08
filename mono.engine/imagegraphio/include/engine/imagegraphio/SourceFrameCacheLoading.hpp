#pragma once

#include <engine/imagegraphio/SourceFrameCache.hpp>

namespace engine::imagegraphio {
	// Checked source loading preserves the complete indexed-array extent, including holes.
	// Refusal leaves the destination untouched. Layout must identify the exact saved text.
	imagegraph::Status DecodeSourceFrameCacheLoading(
		const imagegraph::Node &node,
		const SourceFrameCacheLayoutObservation &observation,
		imagegraph::DataReplayEntry &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	imagegraph::Status DecodeSourceFrameCachesLoading(
		const imagegraph::Document &document,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		imagegraph::DataReplayState &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// Cook complete inventories into native v2 receipts for progressive constructor loading.
	// Input, old destination and candidate share the operation budget, including aliased calls.
	imagegraph::Status CookSourceFrameCachesLoading(
		const imagegraph::Document &source,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		imagegraph::Document &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
}
