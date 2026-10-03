#pragma once

#include <engine/imagegraph/FeedbackHost.hpp>

#include <studio/ImageGraph.hpp>

namespace studio::detail {
	// Counts retained capacities and validates source surfaces without cloning.
	uint64_t ImageGraphPreviewBytes(const ImageGraphPreviewValue &preview) noexcept;
	uint64_t ImageGraphSequenceBytes(const engine::imagegraph::ImageArray &sequence) noexcept;
	bool ValidateImageGraphSequence(
		const engine::imagegraph::ImageArray &sequence, engine::imagegraph::Diagnostic &diagnostic
	);
	bool ValidateImageGraphPreview(
		const ImageGraphPreviewValue &preview, engine::imagegraph::Diagnostic &diagnostic
	);
	// RefreshPreview's single preparation boundary. Both ordinary and stateful
	// outputs use the same validation; a failed replacement retains prior pixels.
	engine::imagegraph::Status PrepareImageGraphPreviewResult(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view outputId,
		engine::imagegraph::EvaluationRequest request,
		engine::imagegraph::CapturedFeedbackHost &feedback,
		uint64_t revision,
		uint64_t inputRevision,
		ImageGraphPreviewValue &preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
