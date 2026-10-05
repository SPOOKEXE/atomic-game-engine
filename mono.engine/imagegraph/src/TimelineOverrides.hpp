#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <span>

namespace engine::imagegraph::detail {
	struct TimelineNodeOverride {
		size_t NodeIndex = 0;
		Node Authored;
	};

	// Only animated nodes in the selected dependency cone own sampled replacements.
	struct TimelineOverrides {
		AllocationReservation Charge;
		std::vector<TimelineNodeOverride> Nodes;
		std::optional<FrameTime> Observation;
		const Node &Find(size_t index, const Node &fallback) const;
	};

	// The caller validates the original document/plan. Both old and new overrides use this live budget.
	// Raw quaternion capture retains animator angles before the property getter converts Euler units.
	Status ResolveTimelineOverrides(
		const Document &document,
		std::span<const uint8_t> needed,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		TimelineOverrides &result,
		Diagnostic &diagnostic,
		std::string_view port = {},
		bool rawSourceQuaternion = false
	);
	// Adds newly needed nodes within the same immutable document/request observation.
	// Already sampled nodes survive; all old/new payloads and replacement tables coexist under budget.
	Status ExtendTimelineOverrides(
		const Document &document,
		std::span<const uint8_t> needed,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		TimelineOverrides &result,
		Diagnostic &diagnostic,
		bool rawSourceQuaternion = false
	);
}
