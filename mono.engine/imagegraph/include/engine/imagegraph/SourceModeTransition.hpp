#pragma once

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupReplay.hpp>

namespace engine::imagegraph {
	struct SourceModeTransition {
		std::string_view NodeId;
		std::string_view Port;
		bool Animated = false;
		FrameTime Time{};
	};
	// The caller supplies a validated source document and replay at this revision. This folds replay
	// effects, changes the selected property's mode and edits its original animator owner atomically.
	// Failure preserves result. The bound includes document, replay, prior result and all replacements.
	// Fresh keys use the source's headless linear default; prior widget key_inter history is not stored.
	// Before publication the host must Compile the candidate: enabling a mode can activate invalid
	// previously inactive track settings. Reject that candidate without changing live owners/history.
	// After acceptance the host must resolve bindings again so nearest getter modes follow the new flag.
	Status ToggleSourceInputMode(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		const SourceModeTransition &transition,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Publishes the mode's authored flags and retained animator replay together.
	// Required when the original physical input was removed while an alias survived.
	Status ToggleSourceInputMode(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		const SourceModeTransition &transition,
		Document &result,
		GroupReplayState &replayResult,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
