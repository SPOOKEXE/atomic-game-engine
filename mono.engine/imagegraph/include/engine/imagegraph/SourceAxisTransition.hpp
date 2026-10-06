#pragma once

// bounded source X/Y edits preserve local flags and independently captured physical animators.

#include <engine/imagegraph/GroupReplay.hpp>

namespace engine::imagegraph {
	// catalogue-backed fixed two-axis carriers accepted by the native transition.
	bool SupportsSourceAxisTransition(const Node &node, std::string_view port);
	// one local separate/combine event. sampling maps stay borrowed until the call returns.
	struct SourceAxisTransition {
		std::string_view NodeId;
		std::string_view Port;
		bool Separated = false;
		// false changes only the flag and leaves uninitialized arrays cold.
		bool SetValue = true;
		// self/node_values expressions borrow the host's retained map, including a known empty map.
		std::optional<std::span<const AuthoredValue>> ObservedInputs = std::nullopt;
		std::string_view ObservedInputOwner{};
	};
	// change the local X/Y flag and its captured physical animators together.
	// separate copies raw components and easing; combine samples the selected unitless getter.
	// flag-only events leave cold storage cold. fresh rows use headless linear constructor easing.
	// callers supply validated source documents and bound replay at this revision.
	// the candidate is compiled before publication; refusal preserves both outputs, including aliases.
	Status ToggleSourceAxes(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		const SourceAxisTransition &transition,
		const EvaluationRequest &request,
		Document &result,
		GroupReplayState &replayResult,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
