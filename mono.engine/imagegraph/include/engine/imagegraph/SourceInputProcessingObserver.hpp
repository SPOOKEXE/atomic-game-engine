#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// Returns the expression context owner using the same inherited and moved-input rules as evaluation.
	std::optional<std::string_view> SourceInputExpressionOwner(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		std::string_view port,
		const GroupReplayState *replay = nullptr
	);
	// Optional synchronous receipt sink for one normally processed source node.
	// The spans expire on return. Observing never requests another evaluation.
	class SourceInputProcessingObserver {
	  public:
		virtual ~SourceInputProcessingObserver() = default;
		virtual std::string_view NodeId() const noexcept = 0;
		// Checked before owner lookup or input copying, including historical replay clocks.
		virtual bool ObservesFrame(FrameTime) const noexcept {
			return true;
		}
		virtual bool ObservesNode(std::string_view nodeId) const noexcept {
			return nodeId == NodeId();
		}
		virtual Status ObserveNode(
			std::string_view,
			FrameTime frame,
			std::span<const EvaluationInputValue> values,
			std::span<const EvaluationInputImage> images,
			std::span<const SnapshotImageArray> arrays,
			Diagnostic &diagnostic,
			uint64_t maximumBytes
		) {
			return Observe(frame, values, images, arrays, diagnostic, maximumBytes);
		}
		// Optional sinks may mark the receipt unavailable while allowing ordinary processing to continue.
		// An empty owner requests release of all receipt storage at a shared budget boundary.
		virtual Status CaptureRefused(std::string_view, Diagnostic &diagnostic) {
			return diagnostic.Code;
		}
		virtual uint64_t RetainedBytes() const noexcept = 0;
		virtual Status Observe(
			FrameTime frame,
			std::span<const EvaluationInputValue> values,
			std::span<const EvaluationInputImage> images,
			std::span<const SnapshotImageArray> arrays,
			Diagnostic &diagnostic,
			uint64_t maximumBytes
		) = 0;
	};
}
