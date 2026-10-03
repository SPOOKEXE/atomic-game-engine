#pragma once
#include <algorithm>
#include <imgui.h>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	using engine::imagegraph::Diagnostic;
	using engine::imagegraph::Document;
	using engine::imagegraph::Node;
	template <class Apply>
	void DrawAnimationTrackPolicy(
		const Document &authored,
		const Node &node,
		std::string_view property,
		Diagnostic &error,
		const Apply &apply
	) {
		const size_t keyCount = static_cast<size_t>(
			std::count_if(authored.Keyframes.begin(), authored.Keyframes.end(), [&](const auto &keyframe) {
				return keyframe.NodeId == node.Id && keyframe.Port == property;
			})
		);
		if (keyCount == 0) return;

		const auto track =
			std::find_if(authored.Tracks.begin(), authored.Tracks.end(), [&](const auto &candidate) {
				return candidate.NodeId == node.Id && candidate.Port == property;
			});
		const bool hasTrack = track != authored.Tracks.end();
		int quaternionSelection = 0;
		if (hasTrack && track->QuaternionMode)
			quaternionSelection = static_cast<int>(*track->QuaternionMode) + 1;
		std::string end = hasTrack ? track->End : "hold";
		int64_t loopRange = hasTrack ? track->LoopRange : -1;
		const std::string section = "Animation (" + std::to_string(keyCount) + " keys)";
		if (!ImGui::TreeNode(section.c_str())) return;

		bool policyChanged = false;
		if (ImGui::BeginCombo("End", end.c_str())) {
			for (const char *choice : {"hold", "loop", "ping", "wrap"}) {
				const bool selected = end == choice;
				if (ImGui::Selectable(choice, selected)) {
					end = choice;
					policyChanged = true;
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::InputScalar("Loop tail (-1 = all)", ImGuiDataType_S64, &loopRange)) {
			loopRange = std::clamp(loopRange, int64_t{-1}, static_cast<int64_t>(keyCount) - 1);
			policyChanged = true;
		}

		const std::string nodeId = node.Id;
		const std::string propertyId(property);
		const bool quaternionKeys =
			std::any_of(authored.Keyframes.begin(), authored.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == nodeId && key.Port == propertyId &&
					   std::holds_alternative<engine::imagegraph::Quaternion>(key.Data);
			});
		if (quaternionKeys) {
			static constexpr const char *modes[]{"Unspecified", "Raw", "Euler degrees"};
			const int selected = quaternionSelection;
			if (ImGui::BeginCombo("Quaternion", modes[std::clamp(selected, 0, 2)])) {
				for (int choice = 0; choice < 3; choice++) {
					if (ImGui::Selectable(modes[choice], choice == selected)) {
						apply([&](Document &document) {
							SetImageGraphTrackQuaternionMode(
								document,
								nodeId,
								propertyId,
								choice == 0 ? std::nullopt : std::optional<int64_t>{choice - 1},
								error
							);
						});
					}
				}
				ImGui::EndCombo();
			}
		}
		const auto savePolicy = [&] {
			apply([&](Document &document) {
				if (SetImageGraphAnimationTrack(document, nodeId, propertyId, end, loopRange, error))
					error = {};
			});
		};
		if (hasTrack && policyChanged) savePolicy();
		if (!hasTrack) {
			ImGui::TextDisabled("No track override. Preview holds the final keyed value.");
			if (ImGui::SmallButton("Add track policy")) savePolicy();
		} else if (ImGui::SmallButton("Remove track policy")) {
			apply([&](Document &document) {
				if (RemoveImageGraphAnimationTrack(document, nodeId, propertyId, error)) error = {};
			});
		}
		ImGui::TreePop();
	}

}
