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
		std::vector<ImageGraphKeyframeIdentity> Selection, PreparedSelection;
		bool Prepared = false;
		std::vector<engine::imagegraph::Keyframe> Clipboard, Originals;
		std::vector<int8_t> ClipboardAxes, OriginalAxes;
		engine::imagegraph::FrameTime Anchor, Destination;
		bool Active = false, Copying = false;
		std::string TargetNode, TargetPort;

		static ImageGraphKeyframeIdentity
		Identity(const engine::imagegraph::Keyframe &key, int8_t axis = -1) {
			return {key.NodeId, key.Port, engine::imagegraph::GetFrameTime(key), axis};
		}
		bool Selected(const engine::imagegraph::Keyframe &key, int8_t axis = -1) const {
			return std::any_of(Selection.begin(), Selection.end(), [&](const auto &identity) {
				return identity.Axis == axis && identity.NodeId == key.NodeId && identity.Port == key.Port &&
					   identity.Time == engine::imagegraph::GetFrameTime(key);
			});
		}
		bool DrawRow(const engine::imagegraph::Keyframe &key, int8_t axis = -1) {
			using namespace engine::imagegraph;
			const auto time = GetFrameTime(key);
			if (axis < -1 || axis > 1 || !ValidFrameTime(time) ||
				key.NodeId.size() > Limits::MaximumTextBytes || key.Port.size() > Limits::MaximumTextBytes)
				return false;
			// The entire authored identity scopes the row, including keys on the same property.
			ImGuiID id = ImGui::GetCurrentWindow()->IDStack.back();
			const auto nodeLength = key.NodeId.size(), portLength = key.Port.size();
			id = ImHashData(&nodeLength, sizeof(nodeLength), id);
			id = ImHashData(key.NodeId.data(), key.NodeId.size(), id);
			id = ImHashData(&portLength, sizeof(portLength), id);
			id = ImHashData(key.Port.data(), key.Port.size(), id);
			id = ImHashData(&axis, sizeof(axis), id);
			id = ImHashData(&time.Tick, sizeof(time.Tick), id);
			const uint64_t fraction = std::bit_cast<uint64_t>(time.Subframe == 0 ? 0.0 : time.Subframe);
			id = ImHashData(&fraction, sizeof(fraction), id);
			id = ImHashData(&time.NegativeFrame, sizeof(time.NegativeFrame), id);
			ImGui::PushOverrideID(id);
			const std::string label = key.NodeId + "." + key.Port + (axis < 0 ? "" : axis == 0 ? ".x" : ".y");
			// grug keep selection in this column so later controls own their clicks.
			const bool clicked = ImGui::Selectable(label.c_str(), Selected(key, axis));
			if (clicked) {
				const auto identity = Identity(key, axis);
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
		// grug capture keys and component selectors together; refusal preserves the previous snapshot.
		static bool CaptureSelection(
			const engine::imagegraph::Document &document,
			std::span<const ImageGraphKeyframeIdentity> selection,
			std::vector<engine::imagegraph::Keyframe> &keys,
			std::vector<int8_t> &axes,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes
		) try {
			using namespace engine::imagegraph;
			const auto refuse = [&](Status code, const char *message) {
				error = {code, {}, {}, message};
				return false;
			};
			if (selection.empty() || selection.size() > Limits::MaximumKeyframes ||
				keys.size() > Limits::MaximumKeyframes || (!axes.empty() && axes.size() != keys.size()))
				return refuse(Status::InvalidValue, "key snapshot has invalid component selectors");
			uint64_t remaining = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
			const uint64_t slots =
				axes.capacity() + selection.size() + (keys.capacity() - keys.size()) * sizeof(Keyframe);
			if (slots > remaining)
				return refuse(Status::LimitExceeded, "key snapshot selectors exceed the payload budget");
			remaining -= slots;
			for (const auto &key : keys) {
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || *bytes > remaining)
					return refuse(Status::LimitExceeded, "retained key snapshot exceeds the payload budget");
				remaining -= *bytes;
			}
			std::vector<int8_t> candidateAxes;
			candidateAxes.reserve(selection.size());
			if (candidateAxes.capacity() - selection.size() > remaining)
				return refuse(
					Status::LimitExceeded, "key snapshot selector capacity exceeds the payload budget"
				);
			remaining -= candidateAxes.capacity() - selection.size();
			for (const auto &identity : selection) {
				if (identity.Axis < -1 || identity.Axis > 1)
					return refuse(Status::InvalidValue, "key snapshot component selector is invalid");
				candidateAxes.push_back(identity.Axis);
			}
			std::vector<Keyframe> candidate;
			if (!CaptureImageGraphKeyframes(document, selection, candidate, error, remaining)) return false;
			keys = std::move(candidate);
			axes = std::move(candidateAxes);
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "key snapshot allocation failed"};
			return false;
		}

		std::optional<uint64_t> Remaining(
			bool includeClipboard,
			bool includeOriginals,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) const {
			using namespace engine::imagegraph;
			uint64_t remaining = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
			const uint64_t pendingBytes = PreparedSelection.capacity() * sizeof(ImageGraphKeyframeIdentity);
			if (pendingBytes > remaining) return std::nullopt;
			remaining -= pendingBytes;
			for (const auto &identity : PreparedSelection) {
				const uint64_t bytes = identity.NodeId.size() + identity.Port.size();
				if (bytes > remaining) return std::nullopt;
				remaining -= bytes;
			}
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
			const uint64_t axisBytes = (includeClipboard ? ClipboardAxes.capacity() : 0) +
									   (includeOriginals ? OriginalAxes.capacity() : 0);
			if (axisBytes > remaining) return std::nullopt;
			remaining -= axisBytes;

			return remaining;
		}
		bool Copy(const engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error) {
			const auto remaining = Remaining(false, true);
			if (!remaining) return false;
			return CaptureSelection(document, Selection, Clipboard, ClipboardAxes, error, *remaining);
		}
		bool Begin(
			const engine::imagegraph::Document &document,
			bool paste,
			const engine::imagegraph::FrameTime &cursor,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			if (!ValidFrameTime(cursor)) return false;
			if (paste) {
				if (Clipboard.empty()) return false;
				if ((!ClipboardAxes.empty() && ClipboardAxes.size() != Clipboard.size()) ||
					std::any_of(ClipboardAxes.begin(), ClipboardAxes.end(), [](int8_t axis) {
						return axis < -1 || axis > 1;
					})) {
					error = {Status::InvalidValue, {}, {}, "clipboard has invalid component selectors"};
					return false;
				}
				const auto budget = Remaining(true, true);
				if (!budget) return false;
				uint64_t remaining = *budget;
				if (ClipboardAxes.size() > remaining) return false;
				remaining -= ClipboardAxes.size();
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
				std::vector<Keyframe> candidate = Clipboard;
				std::vector<int8_t> candidateAxes = ClipboardAxes;
				Originals = std::move(candidate);
				OriginalAxes = std::move(candidateAxes);
			} else {
				const auto remaining = Remaining(true, false);
				if (!remaining ||
					!CaptureSelection(document, Selection, Originals, OriginalAxes, error, *remaining))
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
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "staged key snapshot allocation failed"
			};
			return false;
		}
		// Stage the document without closing the popup or replacing its pinned selection.
		bool PrepareCommit(
			engine::imagegraph::Document &document,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
			std::span<const engine::imagegraph::Keyframe> projectedPins = {}
		) {
			using namespace engine::imagegraph;
			const auto originals =
				projectedPins.empty() ? std::span<const Keyframe>{Originals} : projectedPins;
			Prepared = false;
			const auto budget = Remaining(true, true, maximumBytes);
			if (!Active || !budget) return false;
			if (originals.size() != Originals.size() ||
				(!OriginalAxes.empty() && OriginalAxes.size() != originals.size()) ||
				std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) {
					return axis < -1 || axis > 1;
				})) {
				error = {Status::InvalidValue, {}, {}, "staged keys have invalid component selectors"};
				return false;
			}
			uint64_t remaining = *budget;
			std::vector<ImageGraphKeyframeIdentity> selection;
			if (Copying && !TargetNode.empty()) {
				if (std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) {
						return axis != -1;
					})) {
					error = {
						Status::TypeMismatch, {}, {}, "scalar axis keys require a component paste target"
					};
					return false;
				}
				if (!PasteImageGraphKeyframesToProperty(
						document, originals, Destination, TargetNode, TargetPort, error, remaining
					))
					return false;
			} else {
				for (const auto &key : originals) {
					const uint64_t bytes =
						sizeof(ImageGraphKeyframeIdentity) + key.NodeId.size() + key.Port.size();
					if (bytes > remaining) {
						error = {
							Status::LimitExceeded, {}, {}, "key transfer selection exceeds the payload budget"
						};
						return false;
					}
					remaining -= bytes;
				}
				selection.reserve(originals.size());
				for (size_t index = 0; index < originals.size(); ++index) {
					const auto &key = originals[index];
					FrameTime time;
					if (!ShiftFrameTime(GetFrameTime(key), Anchor, Destination, time)) {
						error = {
							Status::InvalidValue, {}, {}, "shifted key frame exceeds the authored range"
						};
						return false;
					}
					if (time.NegativeFrame) time = {};
					ImageGraphKeyframeIdentity identity{
						key.NodeId, key.Port, time, OriginalAxes.empty() ? int8_t{-1} : OriginalAxes[index]
					};
					if (std::find(selection.begin(), selection.end(), identity) == selection.end())
						selection.push_back(std::move(identity));
				}
				if (!TransferImageGraphKeyframes(
						document, originals, Anchor, Destination, Copying, error, remaining, OriginalAxes
					))
					return false;
			}
			PreparedSelection = std::move(selection);
			Prepared = true;
			return true;
		}
		void PublishCommit() {
			if (!Prepared) return;
			Selection.swap(PreparedSelection);
			Cancel();
		}
		bool Commit(engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error) {
			if (!PrepareCommit(document, error)) return false;
			PublishCommit();
			return true;
		}
		void Cancel() {
			Active = Prepared = false;
			std::vector<engine::imagegraph::Keyframe>().swap(Originals);
			std::vector<int8_t>().swap(OriginalAxes);
			std::vector<ImageGraphKeyframeIdentity>().swap(PreparedSelection);
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
