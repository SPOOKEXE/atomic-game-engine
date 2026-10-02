#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// Native feedback binds image.captured source IDs to graph outputs from the preceding fixed tick.
	struct FeedbackBinding {
		std::string SourceId, OutputId;
		bool operator==(const FeedbackBinding &) const = default;
	};
	struct FeedbackReplayState {
		uint64_t Tick = 0, AuthoringRevision = 0;
		bool Initialized = false;
		std::vector<FeedbackBinding> Bindings;
		std::vector<RequestImageSource> Sources;
	};
	// Reset evaluates frame zero using explicit seed images. Later calls require contiguous integer ticks
	// and the same authoring revision. Each output reads the same old generation; commit is atomic.
	Status ReplayFeedbackFrame(
		const Document &document,
		const Plan &plan,
		std::span<const FeedbackBinding> bindings,
		std::span<const RequestImageSource> seeds,
		const EvaluationRequest &request,
		const FeedbackReplayState &previous,
		uint64_t authoringRevision,
		bool reset,
		FeedbackReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Seek rebuilds from explicit seeds, replaying every tick including the target. Work is bounded.
	Status SeekFeedbackReplay(
		const Document &document,
		const Plan &plan,
		std::span<const FeedbackBinding> bindings,
		std::span<const RequestImageSource> seeds,
		const EvaluationRequest &target,
		uint64_t authoringRevision,
		FeedbackReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumSteps = 4096,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

}
