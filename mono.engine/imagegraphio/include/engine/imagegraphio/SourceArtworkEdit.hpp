#pragma once

#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraphio {
	struct SourceArtworkLayer {
		std::string Name;
		bool Renderable = true;
		bool Loop = true;
	};
	struct SourceArtworkTag {
		std::string Name;
		imagegraph::Colour Color{255, 255, 255, 255};
		uint64_t First = 0;
		uint64_t Last = 0;
	};
	// Prepared file observations remain borrowed. This result contains authoring metadata only.
	struct SourceArtworkMetadata {
		std::vector<SourceArtworkLayer> Layers;
		std::vector<SourceArtworkTag> Tags;
		uint64_t Frames = 0;
	};
	// Exact pinned ASE duplicate-name and frame-zero tag-chunk profile. ORA/Krita retain layer order.
	// Counts, string capacities and borrowed/prior/candidate payload overlap are bounded.
	// Failure preserves the prior result; this function never reads a path or executes a producer.
	imagegraph::Status ReadSourceArtworkMetadata(
		const imagegraph::HostNodeCapture &prepared,
		SourceArtworkMetadata &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// The same ASE profile serves live file readers and derived layer/tag consumers. Source bytes
	// and the inspection tree stay unchanged; controls supply the source layer-loop attributes.
	imagegraph::Status ReadSourceAsepriteMetadata(
		const imagegraph::Node &controls,
		const imagegraph::Value &content,
		SourceArtworkMetadata &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	enum class SourceArtworkAction : uint8_t { GenerateLayers, MatchFrames, ImportTags };
	struct SourceArtworkEditOptions {
		SourceArtworkAction Action = SourceArtworkAction::GenerateLayers;
		bool MatchRegionNames = true;
		bool ReplaceExistingKeys = true;
		uint64_t AuthoringRevision = 0;
		uint64_t NextAuthoringRevision = 0;
		std::optional<std::string_view> CanvasGroup;
	};
	// One atomic source callback transaction over immutable prepared content and bound animator owners.
	// The caller must attest document/input/grant generation before admission. Receipt Authored and
	// exact signed clock are checked here. Borrowed replay/provider pointers are never retained.
	// Successful results are projected authoring state and rebound owners at the next revision.
	imagegraph::Status ApplySourceArtworkEdit(
		const imagegraph::Document &document,
		const imagegraph::HostNodeCapture &prepared,
		const imagegraph::GroupReplayState &replay,
		const SourceArtworkEditOptions &options,
		imagegraph::Document &result,
		imagegraph::GroupReplayState &resultReplay,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
}
