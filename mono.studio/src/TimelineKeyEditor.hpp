#pragma once

#include <algorithm>
#include <bit>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/ImageGraph.hpp>

namespace studio {
	// Selection is session state. A staged move pins the full originals so changes
	// to value, easing or drivers cannot be overwritten by a later Apply.
	struct TimelineKeyEditor {
		std::vector<ImageGraphKeyframeIdentity> Selection;
		std::vector<engine::imagegraph::Keyframe> Clipboard, Originals;
		engine::imagegraph::FrameTime Anchor, Destination;
		bool Active = false, Copying = false;
		std::string TargetNode, TargetPort;

		static ImageGraphKeyframeIdentity Identity(const engine::imagegraph::Keyframe &key) {
			return {key.NodeId, key.Port, engine::imagegraph::GetFrameTime(key)};
		}
		bool Selected(const engine::imagegraph::Keyframe &key) const {
			return std::any_of(Selection.begin(), Selection.end(), [&](const auto &identity) {
				return identity.NodeId == key.NodeId && identity.Port == key.Port &&
					   identity.Time == engine::imagegraph::GetFrameTime(key);
			});
		}
		bool DrawRow(const engine::imagegraph::Keyframe &key) {
			using namespace engine::imagegraph;
			const auto time = GetFrameTime(key);
			if (!ValidFrameTime(time) || key.NodeId.size() > Limits::MaximumTextBytes ||
				key.Port.size() > Limits::MaximumTextBytes)
				return false;
			// The entire authored identity scopes the row, including keys on the same property.
			ImGuiID id = ImGui::GetCurrentWindow()->IDStack.back();
			const auto nodeLength = key.NodeId.size(), portLength = key.Port.size();
			id = ImHashData(&nodeLength, sizeof(nodeLength), id);
			id = ImHashData(key.NodeId.data(), key.NodeId.size(), id);
			id = ImHashData(&portLength, sizeof(portLength), id);
			id = ImHashData(key.Port.data(), key.Port.size(), id);
			id = ImHashData(&time.Tick, sizeof(time.Tick), id);
			const uint64_t fraction = std::bit_cast<uint64_t>(time.Subframe == 0 ? 0.0 : time.Subframe);
			id = ImHashData(&fraction, sizeof(fraction), id);
			id = ImHashData(&time.NegativeFrame, sizeof(time.NegativeFrame), id);
			ImGui::PushOverrideID(id);
			const std::string label = key.NodeId + "." + key.Port;
			const bool clicked =
				ImGui::Selectable(label.c_str(), Selected(key), ImGuiSelectableFlags_SpanAllColumns);
			if (clicked) {
				const auto identity = Identity(key);
				const auto found = std::find(Selection.begin(), Selection.end(), identity);
				if (ImGui::GetIO().KeyShift) {
					if (found != Selection.end())
						Selection.erase(found);
					else if (Selection.size() < Limits::MaximumKeyframes) {
						uint64_t bytes =
							sizeof(ImageGraphKeyframeIdentity) + key.NodeId.size() + key.Port.size();
						for (const auto &entry : Selection)
							bytes +=
								sizeof(ImageGraphKeyframeIdentity) + entry.NodeId.size() + entry.Port.size();
						if (bytes <= Limits::MaximumEvaluationBytes) Selection.push_back(identity);
					}
				} else if (found == Selection.end())
					Selection = {identity};
			}
			ImGui::PopID();
			return clicked;
		}
		std::optional<uint64_t> Remaining(bool includeClipboard, bool includeOriginals) const {
			using namespace engine::imagegraph;
			uint64_t remaining = Limits::MaximumEvaluationBytes;
			const uint64_t spareSelection =
				(Selection.capacity() - Selection.size()) * sizeof(ImageGraphKeyframeIdentity);
			if (spareSelection > remaining) return std::nullopt;
			remaining -= spareSelection;
			for (const auto &identity : Selection) {
				const uint64_t bytes =
					sizeof(ImageGraphKeyframeIdentity) + identity.NodeId.size() + identity.Port.size();
				if (bytes > remaining) return std::nullopt;
				remaining -= bytes;
			}
			for (const auto *keys :
				 {includeClipboard ? &Clipboard : nullptr, includeOriginals ? &Originals : nullptr}) {
				if (!keys) continue;
				for (const auto &key : *keys) {
					const auto bytes = KeyframePayloadBytes(key);
					if (!bytes || *bytes > remaining) return std::nullopt;
					remaining -= *bytes;
				}
			}
			return remaining;
		}
		bool Copy(const engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error) {
			const auto remaining = Remaining(false, true);
			if (!remaining) return false;
			return CaptureImageGraphKeyframes(document, Selection, Clipboard, error, *remaining);
		}
		bool Begin(
			const engine::imagegraph::Document &document,
			bool paste,
			const engine::imagegraph::FrameTime &cursor,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			if (!ValidFrameTime(cursor)) return false;
			if (paste) {
				if (Clipboard.empty()) return false;
				const auto budget = Remaining(true, true);
				if (!budget) return false;
				uint64_t remaining = *budget;
				for (const auto &key : Clipboard) {
					const auto bytes = KeyframePayloadBytes(key);
					if (!bytes || *bytes > remaining) {
						error = {
							Status::LimitExceeded, {}, {}, "clipboard exceeds the staged payload budget"
						};
						return false;
					}
					remaining -= *bytes;
				}
				Originals = Clipboard;
			} else {
				const auto remaining = Remaining(true, false);
				if (!remaining ||
					!CaptureImageGraphKeyframes(document, Selection, Originals, error, *remaining))
					return false;
			}
			Anchor = GetFrameTime(Originals.front());
			for (const auto &key : Originals)
				if (CompareFrameTime(GetFrameTime(key), Anchor) < 0) Anchor = GetFrameTime(key);
			Destination = paste ? cursor : Anchor;
			Copying = paste;
			TargetNode.clear();
			TargetPort.clear();
			Active = true;
			return true;
		}
		bool Commit(engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error) {
			using namespace engine::imagegraph;
			const auto remaining = Remaining(true, true);
			if (!Active || !remaining) return false;
			if (Copying && !TargetNode.empty()) {
				if (!PasteImageGraphKeyframesToProperty(
						document, Originals, Destination, TargetNode, TargetPort, error, *remaining
					))
					return false;
				Selection.clear();
				Cancel();
				return true;
			}
			if (!TransferImageGraphKeyframes(
					document, Originals, Anchor, Destination, Copying, error, *remaining
				))
				return false;
			Selection.clear();
			for (const auto &key : Originals) {
				FrameTime time;
				(void)ShiftFrameTime(GetFrameTime(key), Anchor, Destination, time);
				ImageGraphKeyframeIdentity identity{key.NodeId, key.Port, time};
				if (std::find(Selection.begin(), Selection.end(), identity) == Selection.end())
					Selection.push_back(std::move(identity));
			}
			Cancel();
			return true;
		}
		void Cancel() {
			Active = false;
			std::vector<engine::imagegraph::Keyframe>().swap(Originals);
		}

		template <class Apply>
		void Draw(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::FrameTime &cursor,
			engine::imagegraph::Diagnostic &error,
			const Apply &apply
		) {
			ImGui::BeginDisabled(Selection.empty());
			if (ImGui::Button("Move keys") && Begin(document, false, cursor, error))
				ImGui::OpenPopup("##key-transfer");
			ImGui::SameLine();
			if (ImGui::Button("Copy keys")) (void)Copy(document, error);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Clipboard.empty());
			if (ImGui::Button("Paste keys") && Begin(document, true, cursor, error))
				ImGui::OpenPopup("##key-transfer");
			ImGui::EndDisabled();
			if (ImGui::BeginPopup("##key-transfer")) {
				ImGui::TextUnformatted(Copying ? "Paste at frame" : "Move earliest key to frame");
				if (Copying &&
					ImGui::BeginCombo(
						"Target",
						TargetNode.empty() ? "Original properties" : (TargetNode + "." + TargetPort).c_str()
					)) {
					if (ImGui::Selectable("Original properties", TargetNode.empty())) {
						TargetNode.clear();
						TargetPort.clear();
					}
					for (const auto &node : document.Nodes) {
						const auto *schema = engine::imagegraph::FindSchema(node.Type);
						if (!schema) continue;
						for (const auto &property : schema->Properties) {
							const std::string label = node.Id + "." + std::string(property.Id);
							if (ImGui::Selectable(
									label.c_str(), TargetNode == node.Id && TargetPort == property.Id
								)) {
								TargetNode = node.Id;
								TargetPort = property.Id;
							}
						}
						for (const auto &property : node.DynamicInputs) {
							if (!engine::imagegraph::IsAuthoredValueType(property.Type)) continue;
							const std::string label = node.Id + "." + property.Id;
							if (ImGui::Selectable(
									label.c_str(), TargetNode == node.Id && TargetPort == property.Id
								)) {
								TargetNode = node.Id;
								TargetPort = property.Id;
							}
						}
					}
					ImGui::EndCombo();
				}
				ImGui::InputScalar("Whole frame", ImGuiDataType_U64, &Destination.Tick);
				ImGui::InputDouble("Fraction", &Destination.Subframe, 0, 0, "%.17g");
				ImGui::Checkbox("Negative", &Destination.NegativeFrame);
				if (ImGui::Button("Apply") && apply()) ImGui::CloseCurrentPopup();
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					Cancel();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			} else if (Active)
				Cancel();
		}
	};
}
