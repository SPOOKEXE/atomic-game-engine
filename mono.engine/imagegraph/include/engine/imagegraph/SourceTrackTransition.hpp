#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct SourceTrackTransition {
		std::string_view NodeId;
		std::string_view Port;
		// Null removes the captured combined track override.
		const AnimationTrack *Replacement = nullptr;
		bool SourceInterpolation = false;
	};
	// Changes a combined writer's track and every captured projection atomically.
	// SourceInterpolation also normalizes its combined keys; scalar captures stay unchanged.
	// Failure preserves result. The operation admits live overlap and a bounded Compile.
	Status ApplySourceTrackTransition(
		const Document &document,
		const SourceTrackTransition &transition,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
