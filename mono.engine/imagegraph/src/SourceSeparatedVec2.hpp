#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph::detail {
	const SourceSeparatedVec2Animator *FindSeparatedVec2(const Node &, std::string_view port);
	// Clone bytes use required size; retained bytes include existing capacities. Empty storage is absent.
	std::optional<uint64_t> SeparatedAnimatorBytes(const SourceSeparatedVec2Animator &, bool retained);
	std::optional<uint64_t> SeparatedKeyBytes(const Keyframe &, bool retained);
	std::optional<uint64_t> SeparatedScalarBytes(const SourceScalarAnimator &, bool retained);
	std::optional<uint64_t> SeparatedVec2Bytes(const Node &, bool retained);
	Status ValidateSeparatedVec2(const Node &, size_t &aggregateKeys, Diagnostic &);
	// Shared property modes remain separate from the original animator's writer mode.
	Status SampleSeparatedScalar(
		const SourceScalarAnimator &,
		const AnimationTrack *,
		const TimelineSettings *,
		const EvaluationRequest &,
		bool getterAnimated,
		bool writerAnimated,
		EvaluationBudget &,
		double &,
		Diagnostic &
	);
}
