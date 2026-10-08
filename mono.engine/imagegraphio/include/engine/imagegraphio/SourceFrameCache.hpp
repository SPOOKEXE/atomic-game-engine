#pragma once

#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>

namespace engine::imagegraphio {
	// An observed platform layout is bound to the exact foreign saved payload.
	struct SourceFrameCacheLayoutObservation {
		std::string NodeId;
		std::string DataHash;
		bake::SpriteCacheLayout Layout = bake::SpriteCacheLayout::Rgba8TopDown;
	};
	// Decode one source cache into an owned constructor replay row. Sparse frame
	// indexes and nested arrays survive conversion. Failure preserves the result.
	// Rebinding byte layout requires fresh runtime history; existing rows own their decoded pixels.
	imagegraph::Status DecodeSourceFrameCache(
		const imagegraph::Node &node,
		const SourceFrameCacheLayoutObservation &observation,
		imagegraph::DataReplayEntry &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// Decode explicitly observed saved nodes. Unobserved nodes remain unsupported
	// when selected for evaluation. Publication replaces the complete load ledger.
	imagegraph::Status DecodeSourceFrameCaches(
		const imagegraph::Document &document,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		imagegraph::DataReplayState &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// Cook explicitly observed source payloads into a native authored document.
	// The exact foreign text remains authoritative; stale native receipts refuse playback.
	// Failure preserves the previous document, including when source and result alias.
	imagegraph::Status CookSourceFrameCaches(
		const imagegraph::Document &source,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		imagegraph::Document &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);

}
