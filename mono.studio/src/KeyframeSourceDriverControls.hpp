#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <imgui.h>
#include <type_traits>

namespace studio::detail {
	template <class DrawCurve>
	bool DrawKeyframeSourceDriverValue(
		engine::imagegraph::KeyframeSourceDriver &driver, const DrawCurve &drawCurve
	) {
		using namespace engine::imagegraph;
		return std::visit(
			[&](auto &control) {
				using Control = std::decay_t<decltype(control)>;
				if constexpr (std::is_same_v<Control, KeyframeLinearDriver>) {
					return ImGui::InputDouble("Speed", &control.Speed);
				} else if constexpr (std::is_same_v<Control, KeyframeSnapDriver>) {
					return ImGui::InputDouble("Size", &control.Size);
				} else if constexpr (std::is_same_v<Control, KeyframeBounceDriver> ||
									 std::is_same_v<Control, KeyframeElasticDriver>) {
					bool edited = ImGui::InputScalar("Amount", ImGuiDataType_S64, &control.Amount);
					edited |= ImGui::InputDouble("Spacing", &control.Spacing);
					edited |= ImGui::InputDouble("Curve", &control.Curve);
					return edited;
				} else if constexpr (std::is_same_v<Control, KeyframeAudioDriver>) {
					std::array<char, 256> source{};
					std::copy_n(
						control.SourceId.data(),
						std::min(control.SourceId.size(), source.size() - 1),
						source.data()
					);
					bool edited = ImGui::InputText("Capture source", source.data(), source.size());
					if (edited) control.SourceId = source.data();
					if (ImGui::BeginCombo("Metric", control.Metric.c_str())) {
						for (const char *metric : {"rms", "peak", "mean"})
							if (ImGui::Selectable(metric, control.Metric == metric)) {
								control.Metric = metric;
								edited = true;
							}
						ImGui::EndCombo();
					}
					ImGui::TextUnformatted("Native captured-audio offset at exact tick");
					edited |= ImGui::InputScalar("Channel", ImGuiDataType_U32, &control.Channel);
					edited |= ImGui::InputDouble("Gain", &control.Gain);
					edited |= ImGui::InputDouble("Bias", &control.Bias);
					return edited;
				} else if constexpr (std::is_same_v<Control, KeyframeCurveDriver>) {
					return drawCurve(control.Data);
				} else {
					bool edited = ImGui::InputDouble("Frequency", &control.Frequency);
					edited |= ImGui::InputDouble("Amplitude", &control.Amplitude);
					edited |= ImGui::InputDouble("Phase", &control.Phase);
					edited |= ImGui::InputDouble("Smooth", &control.Smooth);
					return edited;
				}
			},
			driver
		);
	}
}
