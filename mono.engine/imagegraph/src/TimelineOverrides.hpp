#pragma once

#include "EvaluationBudget.hpp"
#include "SourceFrameCacheInputs.hpp"
#include "SourceInputSelection.hpp"

#include <engine/imagegraph/Document.hpp>

#include <span>

namespace engine::imagegraph::detail {
	struct TimelineNodeOverride {
		size_t NodeIndex = 0;
		Node Authored;
		// ports borrow the immutable document/replay for this one evaluation.
		std::vector<std::string_view> SampledPorts;
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
	// Raw source animator capture reads combined storage regardless of any property separation flag.
	Status ResolveTimelineOverrides(
		const Document &document,
		std::span<const uint8_t> needed,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		TimelineOverrides &result,
		Diagnostic &diagnostic,
		std::string_view port = {},
		bool rawSourceQuaternion = false,
		std::span<const uint8_t> getters = {},
		std::span<const SourceFrameCacheInputReads> getterReads = {},
		const TimelineOverrides *previous = nullptr,
		bool rawSourceAnimator = false,
		SourceInputSelection selection = {}
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
		bool rawSourceQuaternion = false,
		std::span<const uint8_t> getters = {},
		std::span<const SourceFrameCacheInputReads> getterReads = {},
		SourceInputSelection selection = {}
	);
}
