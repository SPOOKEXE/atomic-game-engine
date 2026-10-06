#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <span>

namespace engine::imagegraph {
	struct SourceKeyframeEdit {
		const Keyframe *Original = nullptr;
		// Null removes the pinned key. Replacement keeps the original logical socket.
		const Keyframe *Replacement = nullptr;
		bool Copy = false;
	};
	// Edits captured combined writers and their compatibility projections in one transaction.
	// Moves keep physical key IDs; copies reset IDs and drivers. Duplicate alias selections must
	// agree. All originals are removed before insertion; the first moved collision wins.
	// Clocks are used verbatim. Scalar axes require a separate axis-aware editor.
	// Failure preserves result. A bounded Compile validates the candidate before publication.
	Status ApplySourceKeyframeEdits(
		const Document &document,
		std::span<const SourceKeyframeEdit> edits,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
