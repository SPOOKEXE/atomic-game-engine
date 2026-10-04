#pragma once

#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraphio {
	enum class SourceImageFrameKind : uint8_t { Live, Cached, ControlsOnly };
	// One owned source-sprite ledger. Render and authoring use the same raw data;
	// live observations remain distinct from an enabled, previously saved cache.
	struct SourceImageFrameObservation {
		// Prepared controls only; Frames are not undeclared replay outputs.
		imagegraph::HostNodeCapture Controls;
		uint64_t AuthoringRevision = 0;
		uint64_t InputRevision = 0;
		std::vector<imagegraph::Image> Frames;
		SourceImageFrameKind Kind = SourceImageFrameKind::Live;
		// Present only when encoded by the prepared native cache admission path.
		std::optional<std::string> EncodedCache;
		std::optional<bake::SpriteCacheLayout> CacheLayout;
	};
	enum class SourceImageAction : uint8_t { MatchLength, Cache, RemoveCache };
	struct SourceImageEditOptions {
		SourceImageAction Action = SourceImageAction::MatchLength;
		uint64_t AuthoringRevision = 0;
		uint64_t NextAuthoringRevision = 0;
		uint64_t InputRevision = 0;
	};
	// Pure transaction over immutable prepared sprite data and explicit source
	// authoring/replay generations. All old and candidate native capacities are
	// admitted before clone. Failure preserves both outputs and changed.
	// MatchLength counts live sprites and retains explicit source timeline bounds.
	// Cache uses live sprites; RemoveCache retains cache_data and requires no files.
	imagegraph::Status ApplySourceImageEdit(
		const imagegraph::Document &document,
		const SourceImageFrameObservation &prepared,
		const imagegraph::GroupReplayState &replay,
		const SourceImageEditOptions &options,
		imagegraph::Document &result,
		imagegraph::GroupReplayState &resultReplay,
		bool &changed,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
}
