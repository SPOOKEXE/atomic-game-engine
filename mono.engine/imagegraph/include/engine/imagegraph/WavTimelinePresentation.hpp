#pragma once
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
namespace engine::imagegraph {
	// Source timeline observations use original channel zero, independently of Mono
	// and preview gain.
	struct WavTimelinePresentation {
		// Owned sample positions: X is a timeline frame offset, Y is the source
		// channel amplitude.
		std::vector<Vector2> Points;
		// Clamped signed authoring cursor divided by the complete clip duration.
		double Progress = 0;
		// Original source channel count, before the Mono getter's output selection.
		size_t Channels = 0;
		// Complete source clip duration in seconds.
		double Duration = 0;
	};
	// Uses the actual target input resolver. Failure preserves the previous
	// observation. The transaction cap includes the input snapshot and old/new
	// owned point capacities.
	Status ResolveWavTimelinePresentation(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		double framesPerSecond,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	);
	// Samples caller-owned original planes without retaining any borrowed pointer.
	Status BuildWavTimelinePresentation(
		const AudioBit &clip,
		FrameTime frame,
		double framesPerSecond,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	);

} // namespace engine::imagegraph
