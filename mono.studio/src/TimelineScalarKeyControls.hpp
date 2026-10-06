#pragma once

#include "KeyframeSourceDriverControls.hpp"

#include <engine/imagegraph/SourceKeyframeTransition.hpp>

namespace studio::detail {
	// grug publish one pinned row edit; caller closes the row after accepted publication.
	template <class Apply, class DrawCurve>
	bool DrawTimelineScalarKeyControls(
		const engine::imagegraph::Keyframe &key,
		uint64_t maximumBytes,
		engine::imagegraph::Diagnostic &error,
		const Apply &apply,
		const DrawCurve &drawCurve
	) {
		using namespace engine::imagegraph;
		ImGui::TableSetColumnIndex(2);
		bool accepted = false;
		if (ImGui::BeginCombo("##interpolation", key.Interpolation.c_str())) {
			for (const char *choice : {"step", "linear", "cubic", "source"})
				if (ImGui::Selectable(choice, key.Interpolation == choice)) {
					accepted = apply(
						[&](auto &draft, size_t) {
							draft.Interpolation = choice;
							if (draft.Interpolation == "source") {
								if (!draft.Ease) draft.Ease = KeyframeEase{};
							} else
								draft.Ease.reset();
						},
						0
					);
					break;
				}
			ImGui::EndCombo();
		}
		if (accepted) return true;
		for (const bool incoming : {true, false}) {
			ImGui::TableSetColumnIndex(incoming ? 3 : 4);
			if (key.Interpolation != "source" || !key.Ease) {
				ImGui::TextUnformatted("-");
				continue;
			}
			ImGui::PushID(incoming ? "ease-in" : "ease-out");
			auto ease = *key.Ease;
			auto &side = incoming ? ease.InType : ease.OutType;
			auto &handle = incoming ? ease.In : ease.Out;
			bool changed = false;
			if (ImGui::BeginCombo("##type", side.c_str())) {
				for (const char *choice : {"linear", "bezier", "cut"})
					if (ImGui::Selectable(choice, side == choice)) {
						side = choice;
						changed = true;
					}
				ImGui::EndCombo();
			}
			ImGui::SetNextItemWidth(54);
			changed |= ImGui::InputDouble("##x", &handle.X, .05, .25, "%.2f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(54);
			changed |= ImGui::InputDouble("##y", &handle.Y, .05, .25, "%.2f");
			if (changed)
				accepted = apply([&](auto &draft, size_t) { draft.Ease = ease; }, sizeof(KeyframeEase) + 32);
			ImGui::PopID();
			if (accepted) return true;
		}
		ImGui::TableSetColumnIndex(5);
		static constexpr const char *names[]{
			"None", "Linear", "Snap", "Bounce", "Elastic", "Curve", "Sine", "Captured audio"
		};
		const size_t choice = key.SourceDriver ? key.SourceDriver->index() + 1 : key.SineDriver ? 6 : 0;
		if (ImGui::SmallButton(names[choice])) ImGui::OpenPopup("##source-driver");
		if (ImGui::BeginPopup("##source-driver")) {
			if (ImGui::BeginCombo("Driver", names[choice])) {
				for (size_t kind = 0; kind < std::size(names); ++kind)
					if (ImGui::Selectable(names[kind], choice == kind)) {
						if (maximumBytes < 4096) {
							error = {
								Status::LimitExceeded, {}, {}, "scalar driver choice exceeds payload bounds"
							};
							break;
						}
						std::optional<KeyframeSourceDriver> driver;
						switch (kind) {
						case 1:
							driver = KeyframeLinearDriver{};
							break;
						case 2:
							driver = KeyframeSnapDriver{};
							break;
						case 3:
							driver = KeyframeBounceDriver{};
							break;
						case 4:
							driver = KeyframeElasticDriver{};
							break;
						case 5:
							driver = KeyframeCurveDriver{};
							break;
						case 6:
							driver = KeyframeSineDriver{};
							break;
						case 7:
							driver = KeyframeAudioDriver{};
							break;
						default:
							break;
						}
						accepted = apply(
							[&](auto &draft, size_t) {
								draft.SourceDriver = driver;
								draft.SineDriver.reset();
								if (driver) {
									draft.Interpolation = "source";
									if (!draft.Ease) draft.Ease = KeyframeEase{};
								}
							},
							4096
						);
						break;
					}
				ImGui::EndCombo();
			}
			if (!accepted && (key.SourceDriver || key.SineDriver)) {
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || maximumBytes < 4096 || *bytes > (maximumBytes - 4096) / 3) {
					error = {Status::LimitExceeded, {}, {}, "scalar driver controls exceed payload bounds"};
				} else {
					const uint64_t held = 3 * *bytes + 4096;
					auto driver = key.SourceDriver.value_or(
						KeyframeSourceDriver{key.SineDriver.value_or(KeyframeSineDriver{})}
					);
					if (DrawKeyframeSourceDriverValue(driver, drawCurve))
						accepted = apply(
							[&](auto &draft, size_t) {
								if (key.SineDriver && !key.SourceDriver)
									draft.SineDriver = std::get<KeyframeSineDriver>(driver);
								else
									draft.SourceDriver = driver;
							},
							held
						);
					if (!accepted && ImGui::SmallButton("Remove driver"))
						accepted = apply(
							[](auto &draft, size_t) {
								draft.SourceDriver.reset();
								draft.SineDriver.reset();
							},
							held
						);
				}
			}
			ImGui::EndPopup();
		}
		if (accepted) return true;
		ImGui::TableSetColumnIndex(6);
		if (ImGui::BeginCombo("##kind", key.Kind == KeyframeKind::Adder ? "Adder" : "Normal")) {
			for (const auto kind : {KeyframeKind::Normal, KeyframeKind::Adder})
				if (ImGui::Selectable(kind == KeyframeKind::Adder ? "Adder" : "Normal", key.Kind == kind)) {
					accepted = apply([&](auto &draft, size_t) { draft.Kind = kind; }, 0);
					break;
				}
			ImGui::EndCombo();
		}
		return accepted;
	}
}
