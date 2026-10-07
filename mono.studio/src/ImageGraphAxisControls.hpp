#pragma once

#include "ImageGraphAxisObservation.hpp"
#include "ImageGraphGroupHost.hpp"

#include <engine/imagegraph/SourceAxisTransition.hpp>

#include <imgui.h>
#include <optional>

namespace studio::detail {
	inline bool ApplyImageGraphAxisControlWithObservation(
		ImageGraphGroupHost &host,
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		const ImageGraphAxisObservation &observation,
		const ImageGraphAxisObservationIdentity &current,
		engine::imagegraph::SourceAxisTransition transition,
		engine::imagegraph::EvaluationRequest request,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		using namespace engine::imagegraph;
		if (transition.SetValue && !transition.Separated &&
			!BindImageGraphAxisObservation(observation, current, transition, error))
			return false;
		const auto held = observation.RetainedBytes();
		if (!held || *held >= maximumBytes) {
			error = {
				Status::LimitExceeded, {}, {}, "retained source axis map exceeds the action payload budget"
			};
			return false;
		}
		return host.ToggleAxes(
			document, history, current.AuthoringRevision, transition, request, error, maximumBytes - *held
		);
	}
	// the button describes the selected property's local flag, even when its getter delegates.
	inline std::optional<bool> DrawImageGraphAxisControl(
		const engine::imagegraph::Node &node,
		std::string_view port,
		const engine::imagegraph::GroupReplayState &replay
	) {
		using namespace engine::imagegraph;
		if (!SupportsSourceAxisTransition(node, port)) return std::nullopt;
		const auto *overlay = replay.SharedSubtype(node.Id, port);
		bool separated = false;
		if (overlay && overlay->SeparatedVec2)
			separated = overlay->SeparatedVec2->Separated;
		else if (node.SourceSeparatedVec2Animators)
			for (const auto &axes : node.SourceSeparatedVec2Animators->Inputs)
				if (axes.Port == port) separated = axes.Separated;
		if (ImGui::SmallButton(separated ? "Combine X/Y" : "Separate X/Y")) return !separated;
		return std::nullopt;
	}
}
