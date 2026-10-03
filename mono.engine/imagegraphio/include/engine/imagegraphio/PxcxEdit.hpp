#pragma once

// Explicit edits to reversible fields in an existing imported source project.
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>

#include <variant>

namespace engine::imagegraphio {
	struct PxcxNodePositionEdit {
		std::string NodeId;
		imagegraph::Vector2 Position;
	};
	struct PxcxInputValueEdit {
		std::string NodeId;
		std::string Port;
		imagegraph::Value Data;
	};
	struct PxcxKeyframeEdit {
		// Original identity includes NodeId, Port and every signed time component; kind is metadata.
		std::string NodeId;
		std::string Port;
		imagegraph::FrameTime OriginalTime;
		imagegraph::Keyframe Replacement;
	};
	struct PxcxKeyframeInsertEdit {
		std::string NodeId;
		std::string Port;
		imagegraph::Keyframe Replacement;
	};
	struct PxcxKeyframeDeleteEdit {
		std::string NodeId;
		std::string Port;
		imagegraph::FrameTime OriginalTime;
		// Required when deleting the last key: that source project's transient current frame.
		std::optional<imagegraph::FrameTime> CaptureTime = std::nullopt;
	};
	using PxcxEdit = std::variant<
		PxcxNodePositionEdit,
		PxcxInputValueEdit,
		PxcxKeyframeEdit,
		PxcxKeyframeInsertEdit,
		PxcxKeyframeDeleteEdit>;

	// Applies existing-node XY, catalogue values and source key edits atomically.
	// expectedSource must match the retained original bytes; untracked Graph changes are rejected.
	// No-op edits emit identical bytes. Modified JSON retains unknown values and order, not spelling.
	// Payloads share the native 4 MiB array budget. Key moves must retain source record order.
	// Insert/delete require an existing animated expanded track. Surviving records retain their tails.
	// Sequence scans and shifts share a bounded transaction work budget.
	// Insert emits canonical pinned Gradient/Matrix value descriptors; fractional multi-key maps
	// are unsupported. Last-key deletion requires CaptureTime and captures raw setAnim(false) values.
	// A surviving lone key stays expanded to retain metadata that the source serializer compacts.
	// Unknown inverse mappings and conflicting edits fail without replacing out. Retained thumbnails
	// remain reference previews of the source archive; this API does not regenerate them.
	bool WritePxcxEdits(
		const PxcxImport &imported,
		std::span<const std::byte> expectedSource,
		std::span<const PxcxEdit> edits,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	);
}
