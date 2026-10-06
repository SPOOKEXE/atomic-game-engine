#pragma once

#include "ImageGraphCapturedKeyEdit.hpp"
#include "TimelineKeyEditor.hpp"

#include <cmath>
#include <new>

namespace studio {
	// Pin exact source keys while the popup is open. Width edits preserve all
	// other key fields and publish only after the host accepts its transaction.
	struct TimelineEaseEditor {
		std::vector<engine::imagegraph::Keyframe> Originals;
		std::vector<int8_t> OriginalAxes;
		double Delta = 0;
		int Sides = 3;
		bool Active = false;
		uint64_t Revision = 0;

		void Cancel() {
			std::vector<engine::imagegraph::Keyframe>().swap(Originals);
			std::vector<int8_t>().swap(OriginalAxes);
			Active = false;
		}
		bool Begin(
			const engine::imagegraph::Document &document,
			const TimelineKeyEditor &selection,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
			uint64_t revision = 0
		) try {
			using namespace engine::imagegraph;
			if (Active || selection.Active || selection.Selection.empty()) return false;
			const uint64_t count = selection.Selection.size(), keys = document.Keyframes.size();
			const auto remaining = selection.Remaining(true, true);
			if (!remaining || count > Limits::MaximumKeyframes || keys > Limits::MaximumKeyframes ||
				count * (count + 2 * keys) > 64'000'000) {
				error = {Status::LimitExceeded, {}, {}, "easing selection exceeds its capture budget"};
				return false;
			}
			std::vector<Keyframe> captured;
			std::vector<int8_t> axes;
			if (!TimelineKeyEditor::CaptureSelection(
					document, selection.Selection, captured, axes, error, std::min(*remaining, maximumBytes)
				))
				return false;
			for (const auto &key : captured) {
				const auto validSide = [](const auto &side) {
					return side == "linear" || side == "bezier" || side == "cut";
				};
				if (key.Interpolation != "source" || !key.Ease || !validSide(key.Ease->InType) ||
					!validSide(key.Ease->OutType) || !std::isfinite(key.Ease->In.X) ||
					!std::isfinite(key.Ease->In.Y) || !std::isfinite(key.Ease->Out.X) ||
					!std::isfinite(key.Ease->Out.Y)) {
					error = {
						Status::InvalidValue,
						key.NodeId,
						key.Port,
						"selected-key easing needs source interpolation and recorded handles"
					};
					return false;
				}
			}
			Originals = std::move(captured);
			OriginalAxes = std::move(axes);
			Delta = 0;
			Sides = 3;
			Active = true;
			Revision = revision;
			error = {};
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "easing capture allocation failed"};
			return false;
		}
		bool PrepareCommit(
			engine::imagegraph::Document &document,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
			std::span<const engine::imagegraph::Keyframe> projectedPins = {}
		) const {
			using namespace engine::imagegraph;
			const auto originals =
				projectedPins.empty() ? std::span<const Keyframe>{Originals} : projectedPins;
			if (!Active || !std::isfinite(Delta) || Sides < 1 || Sides > 3) {
				error = {Status::InvalidValue, {}, {}, "easing needs finite width and a selected side"};
				return false;
			}
			if (originals.size() > Limits::MaximumKeyframes ||
				document.Keyframes.size() > Limits::MaximumKeyframes ||
				2 * uint64_t(originals.size()) * document.Keyframes.size() > 64'000'000) {
				error = {Status::LimitExceeded, {}, {}, "easing comparison exceeds its work budget"};
				return false;
			}
			if (originals.size() != Originals.size() ||
				(!OriginalAxes.empty() && OriginalAxes.size() != originals.size()) ||
				std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) {
					return axis < -1 || axis > 1;
				})) {
				error = {Status::InvalidValue, {}, {}, "easing component pins are invalid"};
				return false;
			}
			if (document.SourceAnimators ||
				std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) { return axis >= 0; }))
				return EditCapturedImageGraphKeys(
					document,
					originals,
					[](const auto &, size_t) { return true; },
					[&](auto &key, size_t) {
						auto &ease = *key.Ease;
						if (Sides & 2) {
							ease.In.X = std::clamp(ease.In.X + Delta, 0.0, 2.0);
							ease.InType = ease.In.X == 0 ? "linear" : "bezier";
						}
						if (Sides & 1) {
							ease.Out.X = std::clamp(ease.Out.X + Delta, 0.0, 2.0);
							ease.OutType = ease.Out.X == 0 ? "linear" : "bezier";
						}
					},
					error,
					0,
					(Originals.capacity() - Originals.size()) * sizeof(Keyframe) + OriginalAxes.capacity(),
					maximumBytes,
					OriginalAxes
				);
			// Validate every full original before the first staged mutation.
			for (const auto &original : originals) {
				if (std::find(document.Keyframes.begin(), document.Keyframes.end(), original) ==
					document.Keyframes.end()) {
					error = {
						Status::InvalidValue,
						original.NodeId,
						original.Port,
						"easing original changed while editing"
					};
					return false;
				}
			}
			for (const auto &original : originals) {
				auto &key = *std::find(document.Keyframes.begin(), document.Keyframes.end(), original);
				auto &ease = *key.Ease;
				if (Sides & 2) {
					ease.In.X = std::clamp(ease.In.X + Delta, 0.0, 2.0);
					ease.InType = ease.In.X == 0 ? "linear" : "bezier";
				}
				if (Sides & 1) {
					ease.Out.X = std::clamp(ease.Out.X + Delta, 0.0, 2.0);
					ease.OutType = ease.Out.X == 0 ? "linear" : "bezier";
				}
			}
			error = {};
			return true;
		}
		template <class Apply>
		void Draw(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			const TimelineKeyEditor &selection,
			engine::imagegraph::Diagnostic &error,
			const Apply &apply
		) {
			ImGui::BeginDisabled(selection.Selection.empty() || selection.Active);
			if (ImGui::Button("Ease keys") &&
				Begin(
					document, selection, error, engine::imagegraph::Limits::MaximumEvaluationBytes, revision
				))
				ImGui::OpenPopup("##selected-key-ease");
			ImGui::EndDisabled();
			if (ImGui::BeginPopup("##selected-key-ease")) {
				if (Active && Revision != revision) {
					error = {
						engine::imagegraph::Status::InvalidValue,
						{},
						{},
						"easing document changed while editing"
					};
					Cancel();
					ImGui::CloseCurrentPopup();
					ImGui::EndPopup();
					return;
				}
				const char *side = Sides == 3 ? "Both" : Sides == 2 ? "Incoming" : "Outgoing";
				if (ImGui::BeginCombo("Sides", side)) {
					for (int choice : {3, 2, 1}) {
						const char *label = choice == 3 ? "Both" : choice == 2 ? "Incoming" : "Outgoing";
						if (ImGui::Selectable(label, Sides == choice)) Sides = choice;
					}
					ImGui::EndCombo();
				}
				ImGui::InputDouble("Width delta", &Delta, .1, .5, "%.6g");
				if (ImGui::Button("Apply")) {
					if (apply()) {
						Cancel();
						ImGui::CloseCurrentPopup();
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					Cancel();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			if (Active && !ImGui::IsPopupOpen("##selected-key-ease")) Cancel();
		}
	};
}
