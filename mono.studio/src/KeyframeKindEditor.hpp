#pragma once

#include <algorithm>
#include <imgui.h>
#include <studio/ImageGraph.hpp>

namespace studio {

	// A popup stages only metadata. Exact identity survives unrelated key-vector edits;
	// a changed marker or removed target must be reopened before it can be replaced.
	struct KeyframeKindEditor {
		std::string NodeId, Port;
		engine::imagegraph::FrameTime Time;
		engine::imagegraph::KeyframeKind Original = engine::imagegraph::KeyframeKind::Normal;
		engine::imagegraph::KeyframeKind Draft = engine::imagegraph::KeyframeKind::Normal;
		bool Active = false;

		bool Begin(const engine::imagegraph::Document &document, size_t index) {
			using namespace engine::imagegraph;
			if (index >= document.Keyframes.size() || document.Keyframes.size() > Limits::MaximumKeyframes)
				return false;
			const auto &key = document.Keyframes[index];
			if (key.NodeId.size() > Limits::MaximumTextBytes || key.Port.size() > Limits::MaximumTextBytes ||
				!ValidFrameTime(GetFrameTime(key)) ||
				(key.Kind != KeyframeKind::Normal && key.Kind != KeyframeKind::Adder))
				return false;
			NodeId = key.NodeId;
			Port = key.Port;
			Time = GetFrameTime(key);
			Original = Draft = key.Kind;
			Active = true;
			return true;
		}
		bool Select(engine::imagegraph::KeyframeKind kind) {
			using engine::imagegraph::KeyframeKind;
			if (!Active || (kind != KeyframeKind::Normal && kind != KeyframeKind::Adder)) return false;
			Draft = kind;
			return true;
		}
		bool Targets(const engine::imagegraph::Keyframe &key) const {
			return Active && NodeId == key.NodeId && Port == key.Port &&
				   Time == engine::imagegraph::GetFrameTime(key);
		}
		bool PrepareCommit(
			engine::imagegraph::Document &document,
			engine::imagegraph::Diagnostic &error,
			uint64_t availableBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) const {
			const auto key =
				std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &entry) {
					return Targets(entry);
				});
			if (key == document.Keyframes.end() || key->Kind != Original) {
				error = {
					engine::imagegraph::Status::InvalidValue,
					NodeId,
					Port,
					"key changed while its kind editor was open"
				};
				return false;
			}
			if (!SetImageGraphKeyframeKind(
					document,
					static_cast<size_t>(key - document.Keyframes.begin()),
					Draft,
					error,
					availableBytes
				))
				return false;
			return true;
		}
		bool Commit(engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error) {
			if (!PrepareCommit(document, error)) return false;
			Cancel();
			return true;
		}
		// Apply is supplied by the composer so the same document transaction owns undo and preview
		// invalidation.
		template <class Apply>
		void Draw(const engine::imagegraph::Document &document, size_t index, const Apply &apply) {
			using engine::imagegraph::KeyframeKind;
			if (index >= document.Keyframes.size()) return;
			const auto &key = document.Keyframes[index];
			const char *label = key.Kind == KeyframeKind::Adder ? "Adder" : "Normal";
			if (ImGui::SmallButton(label) && Begin(document, index)) ImGui::OpenPopup("##key-kind");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Native marker editor. Both kinds use ordinary key evaluation.");
			if (ImGui::BeginPopup("##key-kind")) {
				if (ImGui::RadioButton("Normal", Draft == KeyframeKind::Normal))
					(void)Select(KeyframeKind::Normal);
				ImGui::SameLine();
				if (ImGui::RadioButton("Adder", Draft == KeyframeKind::Adder))
					(void)Select(KeyframeKind::Adder);
				if (ImGui::Button("Apply") && apply()) ImGui::CloseCurrentPopup();
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					Cancel();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			} else if (Targets(key))
				Cancel();
		}
		void Cancel() {
			Active = false;
		}
	};
}
