#pragma once

#include "ImageGraphKeyPinProjection.hpp"
#include "TimelineKeyEditor.hpp"

namespace studio::detail {
	// Scalar rows borrow the document. Return immediately after a published edit invalidates them.
	template <class Select, class Apply>
	bool DrawTimelineScalarKeys(
		const engine::imagegraph::Document &document,
		TimelineKeyEditor &editor,
		engine::imagegraph::Diagnostic &diagnostic,
		const Select &select,
		const Apply &apply
	) {
		using namespace engine::imagegraph;
		for (const auto &node : document.Nodes) {
			if (!node.SourceSeparatedVec2Animators) continue;
			for (const auto &input : node.SourceSeparatedVec2Animators->Inputs) {
				if (!input.Initialized) continue;
				for (int8_t axis = 0; axis < 2; ++axis)
					for (const auto &key : input.Axes[size_t(axis)].Keys) {
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						if (editor.DrawRow(key, axis)) select(key);
						const ImGuiID rowId = ImGui::GetItemID();
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%.20Lg", FrameTimeToReal(GetFrameTime(key)));
						ImGui::TableSetColumnIndex(2);
						ImGui::TextUnformatted(key.Interpolation.c_str());
						if (key.Ease) {
							ImGui::TableSetColumnIndex(3);
							ImGui::TextUnformatted(key.Ease->InType.c_str());
							ImGui::TableSetColumnIndex(4);
							ImGui::TextUnformatted(key.Ease->OutType.c_str());
						}
						ImGui::TableSetColumnIndex(5);
						ImGui::TextUnformatted(key.SourceDriver || key.SineDriver ? "Driven" : "None");
						ImGui::TableSetColumnIndex(6);
						ImGui::TextUnformatted(key.Kind == KeyframeKind::Adder ? "Adder" : "Normal");
						ImGui::TableSetColumnIndex(7);
						ImGui::PushOverrideID(rowId);
						const bool remove = ImGui::SmallButton("Delete");
						ImGui::PopID();
						if (!remove) continue;
						const bool changed =
							apply([&](Document &candidate,
									  uint64_t availableBytes = Limits::MaximumEvaluationBytes) {
								return WithImageGraphProjectedKeyPins(
									document,
									candidate,
									{&key, 1},
									availableBytes,
									[&](std::span<const Keyframe> pins, uint64_t remaining) {
										const SourceKeyframeEdit edit{&pins.front(), nullptr, false, axis};
										return ApplySourceKeyframeEdits(
												   candidate, {&edit, 1}, candidate, diagnostic, remaining
											   ) == Status::Ok;
									},
									diagnostic,
									{&axis, 1}
								);
							});
						if (changed) return true;
					}
			}
		}
		return false;
	}
}
