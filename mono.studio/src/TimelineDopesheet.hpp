#pragma once

#include "TimelineKeyActions.hpp"
#include "TimelineKeyEditor.hpp"

#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <new>

namespace studio {
	// The display cache contains identities and marker geometry, never authored values.
	// Gestures pin bounded full keys; each accepted edit owns one host transaction.
	struct TimelineDopesheet {
		struct Track {
			std::string NodeId, Port;
		};
		struct Marker {
			size_t Key = 0, Row = 0;
			ImVec2 Position;
		};
		std::vector<Track> Tracks;
		std::optional<Track> FocusedTrack;
		std::vector<Marker> Markers;
		std::vector<engine::imagegraph::Keyframe> Originals;
		std::vector<engine::imagegraph::FrameTime> Destinations;
		std::vector<ImageGraphKeyframeIdentity> BoxSelection, PreparedSelection;
		bool Prepared = false;
		uint64_t Revision = std::numeric_limits<uint64_t>::max();
		double PixelsPerFrame = 20, PanX = 0, PanY = 0;
		ImVec2 BoxStart, BoxEnd;
		engine::imagegraph::FrameTime Anchor, Fixed;
		bool Dragging = false, Scaling = false, Copying = false, Boxing = false, Deleting = false,
			 TargetsValid = true;
		bool Transforming = false, KeyboardCopy = false;
		TimelineKeyAction StagedAction = TimelineKeyAction::Quantize;
		engine::imagegraph::FrameTime MouseAnchor;

		std::optional<uint64_t> CacheBytes() const {
			using namespace engine::imagegraph;
			uint64_t bytes =
				Tracks.capacity() * sizeof(Track) + Markers.capacity() * sizeof(Marker) +
				Destinations.capacity() * sizeof(FrameTime) +
				(BoxSelection.capacity() + PreparedSelection.capacity()) * sizeof(ImageGraphKeyframeIdentity);
			if (FocusedTrack) bytes += FocusedTrack->NodeId.capacity() + FocusedTrack->Port.capacity() + 2;
			for (const auto &track : Tracks)
				bytes += track.NodeId.size() + track.Port.size();
			for (const auto &identity : BoxSelection)
				bytes += identity.NodeId.size() + identity.Port.size();
			for (const auto &identity : PreparedSelection)
				bytes += identity.NodeId.size() + identity.Port.size();
			for (const auto &key : Originals) {
				const auto payload = KeyframePayloadBytes(key);
				if (!payload || *payload > Limits::MaximumEvaluationBytes ||
					bytes > Limits::MaximumEvaluationBytes - *payload)
					return std::nullopt;
				bytes += *payload;
			}
			if (bytes > Limits::MaximumEvaluationBytes) return std::nullopt;
			return bytes;
		}
		bool FocusTrack(
			const Track &track, const TimelineKeyEditor &editor, engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			if (FocusedTrack && FocusedTrack->NodeId == track.NodeId && FocusedTrack->Port == track.Port)
				return true;
			const auto budget = editor.Remaining(true, true), held = CacheBytes();
			// The old focus remains alive until the fully admitted candidate is
			// published.
			const uint64_t candidateBytes =
				sizeof(Track) + track.NodeId.capacity() + track.Port.capacity() + 2;
			const auto refuse = [&] {
				error = {Status::LimitExceeded, {}, {}, "timeline focus exceeds the key payload budget"};
				return false;
			};
			if (!budget || !held || *held > *budget || candidateBytes > *budget - *held) return refuse();
			try {
				Track candidate{track.NodeId, track.Port};
				const uint64_t actual =
					sizeof(Track) + candidate.NodeId.capacity() + candidate.Port.capacity() + 2;
				if (actual > *budget - *held) return refuse();
				FocusedTrack = std::move(candidate);
				return true;
			} catch (const std::bad_alloc &) {
				return refuse();
			}
		}
		bool UpdateBox(
			const engine::imagegraph::Document &document,
			TimelineKeyEditor &editor,
			const ImVec2 &a,
			const ImVec2 &b,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			const auto budget = editor.Remaining(true, true), retained = CacheBytes();
			if (!budget || !retained || *retained > *budget) return false;
			uint64_t remaining = *budget - *retained;
			const auto inside = [&](const Marker &marker) {
				return marker.Position.x >= a.x && marker.Position.x <= b.x && marker.Position.y >= a.y &&
					   marker.Position.y <= b.y;
			};
			const auto saved = [&](const Keyframe &key) {
				return std::any_of(BoxSelection.begin(), BoxSelection.end(), [&](const auto &id) {
					return id.NodeId == key.NodeId && id.Port == key.Port && id.Time == GetFrameTime(key);
				});
			};
			size_t count = BoxSelection.size();
			for (const auto &id : BoxSelection) {
				const uint64_t bytes = sizeof(ImageGraphKeyframeIdentity) + id.NodeId.size() + id.Port.size();
				if (bytes > remaining) return false;
				remaining -= bytes;
			}
			for (const auto &marker : Markers)
				if (marker.Key < document.Keyframes.size() && inside(marker) &&
					!saved(document.Keyframes[marker.Key])) {
					const auto &key = document.Keyframes[marker.Key];
					const uint64_t bytes =
						sizeof(ImageGraphKeyframeIdentity) + key.NodeId.size() + key.Port.size();
					if (bytes > remaining || count >= Limits::MaximumKeyframes) {
						error = {
							Status::LimitExceeded, {}, {}, "key selection exceeds the timeline payload budget"
						};
						return false;
					}
					remaining -= bytes;
					++count;
				}
			std::vector<ImageGraphKeyframeIdentity> candidate;
			candidate.reserve(count);
			candidate.insert(candidate.end(), BoxSelection.begin(), BoxSelection.end());
			for (const auto &marker : Markers)
				if (marker.Key < document.Keyframes.size() && inside(marker) &&
					!saved(document.Keyframes[marker.Key]))
					candidate.push_back(TimelineKeyEditor::Identity(document.Keyframes[marker.Key]));
			editor.Selection = std::move(candidate);
			return true;
		}
		bool Rebuild(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			const TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			if (Revision == revision) return true;
			const auto budget = editor.Remaining(true, true);
			uint64_t remaining = budget.value_or(0);
			const auto retained = CacheBytes();
			if (!retained || *retained > remaining) return false;
			remaining -= *retained;
			if (document.Keyframes.size() > Limits::MaximumKeyframes ||
				document.Keyframes.size() *
						(sizeof(Marker) + sizeof(Track) +
						 sizeof(std::pair<std::pair<std::string_view, std::string_view>, size_t>)) >
					remaining)
				return false;
			remaining -= document.Keyframes.size() *
						 (sizeof(Marker) + sizeof(Track) +
						  sizeof(std::pair<std::pair<std::string_view, std::string_view>, size_t>));
			for (const auto &key : document.Keyframes) {
				if (!ValidFrameTime(GetFrameTime(key)) || key.NodeId.size() > Limits::MaximumTextBytes ||
					key.Port.size() > Limits::MaximumTextBytes ||
					key.NodeId.size() + key.Port.size() > remaining) {
					error = {
						Status::LimitExceeded, {}, {}, "timeline display exceeds the key payload budget"
					};
					return false;
				}
				remaining -= key.NodeId.size() + key.Port.size();
			}
			Tracks.clear();
			Markers.clear();
			std::map<std::pair<std::string_view, std::string_view>, size_t> rows;
			for (size_t index = 0; index < document.Keyframes.size(); ++index) {
				const auto &key = document.Keyframes[index];
				const auto [entry, inserted] = rows.emplace(
					std::pair<std::string_view, std::string_view>{key.NodeId, key.Port}, Tracks.size()
				);
				if (inserted) Tracks.push_back({key.NodeId, key.Port});
				Markers.push_back({index, entry->second, {}});
			}
			Revision = revision;
			return true;
		}
		void Cancel() {
			Dragging = Scaling = Copying = Boxing = Deleting = Prepared = Transforming = KeyboardCopy = false;
			std::vector<engine::imagegraph::Keyframe>().swap(Originals);
			std::vector<engine::imagegraph::FrameTime>().swap(Destinations);
			std::vector<ImageGraphKeyframeIdentity>().swap(BoxSelection);
			std::vector<ImageGraphKeyframeIdentity>().swap(PreparedSelection);
		}
		bool SelectAll(
			const engine::imagegraph::Document &document,
			TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			const auto budget = editor.Remaining(true, true), held = CacheBytes();
			if (!budget || !held || *held > *budget) {
				error = {Status::LimitExceeded, {}, {}, "key selection exceeds the timeline payload budget"};
				return false;
			}
			uint64_t remaining = *budget - *held;
			for (const auto &marker : Markers) {
				if (marker.Key >= document.Keyframes.size()) {
					error = {Status::InvalidValue, {}, {}, "key selection display is stale"};
					return false;
				}
				const auto &key = document.Keyframes[marker.Key];
				const uint64_t bytes =
					sizeof(ImageGraphKeyframeIdentity) + key.NodeId.size() + key.Port.size();
				if (bytes > remaining) {
					error = {
						Status::LimitExceeded, {}, {}, "key selection exceeds the timeline payload budget"
					};
					return false;
				}
				remaining -= bytes;
			}
			std::vector<ImageGraphKeyframeIdentity> selected;
			selected.reserve(Markers.size());
			for (const auto &marker : Markers)
				selected.push_back(TimelineKeyEditor::Identity(document.Keyframes[marker.Key]));
			editor.Selection = std::move(selected);
			error = {};
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "key selection allocation failed"};
			return false;
		}
		bool BeginAction(
			const engine::imagegraph::Document &document,
			const TimelineKeyEditor &editor,
			TimelineKeyAction action,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			if (Dragging || Boxing || Deleting || Transforming || editor.Active || editor.Selection.empty())
				return false;
			const auto budget = editor.Remaining(true, true), held = CacheBytes();
			if (!budget || !held || *held > *budget || editor.Selection.size() > Limits::MaximumKeyframes ||
				document.Keyframes.size() > Limits::MaximumKeyframes ||
				uint64_t(editor.Selection.size()) *
						(editor.Selection.size() + 2 * document.Keyframes.size()) >
					64'000'000) {
				error = {Status::LimitExceeded, {}, {}, "key action exceeds timeline payload or work bound"};
				return false;
			}
			if (!CaptureImageGraphKeyframes(document, editor.Selection, Originals, error, *budget - *held))
				return false;
			const auto captured = CacheBytes();
			if (!captured || *captured > *budget ||
				!PrepareTimelineKeyDestinations(
					Originals, action, Destinations, error, *budget - *captured
				)) {
				Cancel();
				return false;
			}
			StagedAction = action;
			Transforming = TargetsValid = true;
			return true;
		}
		bool BeginDeletion(
			const engine::imagegraph::Document &document,
			const TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			if (Dragging || Boxing || Deleting || editor.Active || editor.Selection.empty()) return false;
			const uint64_t selected = editor.Selection.size(), keys = document.Keyframes.size();
			if (selected > Limits::MaximumKeyframes || keys > Limits::MaximumKeyframes ||
				selected * (selected + 2 * keys) > 64'000'000) {
				error = {Status::LimitExceeded, {}, {}, "key deletion capture exceeds the work bound"};
				return false;
			}
			const auto budget = editor.Remaining(true, true), retained = CacheBytes();
			if (!budget || !retained || *retained > *budget) {
				error = {Status::LimitExceeded, {}, {}, "key deletion exceeds the timeline payload budget"};
				return false;
			}
			if (!CaptureImageGraphKeyframes(
					document, editor.Selection, Originals, error, *budget - *retained
				))
				return false;
			Deleting = true;
			return true;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "key deletion capture allocation failed"
			};
			return false;
		}
		bool Begin(
			const engine::imagegraph::Document &document,
			TimelineKeyEditor &editor,
			size_t index,
			bool scale,
			bool copy,
			engine::imagegraph::Diagnostic &error,
			bool keepCopySelection = false
		) {
			using namespace engine::imagegraph;
			if (index >= document.Keyframes.size()) return false;
			const auto &key = document.Keyframes[index];
			if ((copy && !keepCopySelection) || !editor.Selected(key))
				editor.Selection = {TimelineKeyEditor::Identity(key)};
			const auto budget = editor.Remaining(true, true);
			const auto retained = CacheBytes();
			const uint64_t clocks = editor.Selection.size() * sizeof(FrameTime);
			if (!budget || !retained || *retained > *budget || clocks > *budget - *retained ||
				!CaptureImageGraphKeyframes(
					document, editor.Selection, Originals, error, *budget - *retained - clocks
				))
				return false;
			Anchor = GetFrameTime(key);
			Fixed = GetFrameTime(Originals.front());
			FrameTime first = Fixed, last = Fixed;
			for (const auto &original : Originals) {
				const auto time = GetFrameTime(original);
				if (CompareFrameTime(time, first) < 0) first = time;
				if (CompareFrameTime(time, last) > 0) last = time;
			}
			Fixed = Anchor == first ? last : first;
			if (scale && Fixed == Anchor) {
				Cancel();
				return false;
			}
			Destinations.clear();
			Destinations.reserve(Originals.size());
			for (const auto &original : Originals)
				Destinations.push_back(GetFrameTime(original));
			Dragging = true;
			Scaling = scale;
			Copying = copy;
			TargetsValid = true;
			return true;
		}
		bool Update(const engine::imagegraph::FrameTime &endpoint, engine::imagegraph::Diagnostic &error) {
			using namespace engine::imagegraph;
			TargetsValid = false;
			if (!Dragging || !ValidFrameTime(endpoint)) return false;
			const auto transform = [&](size_t index, FrameTime &time) {
				return Scaling ? ScaleFrameTime(GetFrameTime(Originals[index]), Fixed, Anchor, endpoint, time)
							   : ShiftFrameTime(GetFrameTime(Originals[index]), Anchor, endpoint, time);
			};
			for (size_t index = 0; index < Originals.size(); ++index) {
				FrameTime time;
				if (!transform(index, time)) {
					error = {Status::InvalidValue, {}, {}, "key gesture exceeds the authored frame range"};
					return false;
				}
			}
			for (size_t index = 0; index < Originals.size(); ++index)
				(void)transform(index, Destinations[index]);
			TargetsValid = true;
			error = {};
			return true;
		}

		// Prepare only the staged document. Selection publishes after the host
		// retains its undo entry.
		bool PrepareCommit(
			engine::imagegraph::Document &document,
			const TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			Prepared = false;
			const auto budget = editor.Remaining(true, true);
			uint64_t remaining = budget.value_or(0);
			const auto retained = CacheBytes();
			if ((!Dragging && !Deleting && !Transforming) || (!Deleting && !TargetsValid) || !retained ||
				*retained > remaining)
				return false;
			if (Deleting && (Originals.empty() || document.Keyframes.size() > Limits::MaximumKeyframes ||
							 Originals.size() > Limits::MaximumKeyframes ||
							 uint64_t(Originals.size()) * document.Keyframes.size() * 2 > 64'000'000)) {
				error = {Status::LimitExceeded, {}, {}, "key deletion exceeds the work bound"};
				return false;
			}
			if (!document.SourceAnimators)
				for (const auto &original : Originals)
					if (std::find(document.Keyframes.begin(), document.Keyframes.end(), original) ==
						document.Keyframes.end()) {
						error = {
							Status::InvalidValue,
							original.NodeId,
							original.Port,
							"key changed during its timeline gesture"
						};
						return false;
					}
			if (Deleting && document.SourceAnimators) {
				remaining -= *retained;
				const uint64_t scratch = Originals.size() * sizeof(SourceKeyframeEdit);
				if (scratch > remaining) {
					error = {
						Status::LimitExceeded,
						{},
						{},
						"source key deletion scratch exceeds the payload budget"
					};
					return false;
				}
				std::vector<SourceKeyframeEdit> edits;
				edits.reserve(Originals.size());
				for (const auto &original : Originals)
					edits.push_back({&original});
				Document candidate;
				if (ApplySourceKeyframeEdits(document, edits, candidate, error, remaining - scratch) !=
					Status::Ok)
					return false;
				document = std::move(candidate);
				std::vector<ImageGraphKeyframeIdentity>{}.swap(PreparedSelection);
				Prepared = true;
				return true;
			}
			if (Deleting) {
				std::erase_if(document.Keyframes, [&](const auto &key) {
					return std::any_of(Originals.begin(), Originals.end(), [&](const auto &original) {
						return key.NodeId == original.NodeId && key.Port == original.Port &&
							   GetFrameTime(key) == GetFrameTime(original);
					});
				});
				std::vector<ImageGraphKeyframeIdentity>{}.swap(PreparedSelection);
				Prepared = true;
				return true;
			}
			remaining -= *retained;
			for (const auto &original : Originals) {
				const uint64_t bytes =
					sizeof(ImageGraphKeyframeIdentity) + original.NodeId.size() + original.Port.size();
				if (bytes > remaining) {
					error = {
						Status::LimitExceeded, {}, {}, "key gesture selection exceeds the payload budget"
					};
					return false;
				}
				remaining -= bytes;
			}
			std::vector<ImageGraphKeyframeIdentity> selection;
			selection.reserve(Originals.size());
			for (size_t index = 0; index < Originals.size(); ++index) {
				ImageGraphKeyframeIdentity identity{
					Originals[index].NodeId, Originals[index].Port, Destinations[index]
				};
				if (std::find(selection.begin(), selection.end(), identity) == selection.end())
					selection.push_back(std::move(identity));
			}
			if (Transforming && StagedAction == TimelineKeyAction::Distribute) {
				// Source distribute also sorts the selected list. Keep equal-time selection order.
				const uint64_t scratch = selection.size() * sizeof(ImageGraphKeyframeIdentity);
				if (scratch > remaining) {
					error = {
						Status::LimitExceeded, {}, {}, "distributed selection sort exceeds the payload budget"
					};
					return false;
				}
				remaining -= scratch;
				std::stable_sort(selection.begin(), selection.end(), [](const auto &a, const auto &b) {
					return CompareFrameTime(a.Time, b.Time) < 0;
				});
			}
			if (!RetimeImageGraphKeyframes(
					document, Originals, Destinations, Copying, error, remaining, !Transforming
				))
				return false;
			PreparedSelection = std::move(selection);
			Prepared = true;
			return true;
		}
		void PublishCommit(TimelineKeyEditor &editor) {
			if (!Prepared) return;
			editor.Selection.swap(PreparedSelection);
			Cancel();
			Revision = std::numeric_limits<uint64_t>::max();
		}
		bool Commit(
			engine::imagegraph::Document &document,
			TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) {
			if (!PrepareCommit(document, editor, error)) return false;
			PublishCommit(editor);
			return true;
		}

		template <class Apply>
		void Draw(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			TimelineKeyEditor &editor,
			const engine::imagegraph::FrameTime &cursor,
			engine::imagegraph::Diagnostic &error,
			const Apply &apply,
			const std::function<bool()> &paste = {}
		) {
			using namespace engine::imagegraph;
			if (!Rebuild(document, revision, editor, error)) {
				Cancel();
				return;
			}
			std::optional<TimelineKeyAction> requestedAction;
			if (ImGui::Button("Key actions")) ImGui::OpenPopup("##dopesheet-actions");
			if (ImGui::BeginPopup("##dopesheet-actions")) {
				const std::pair<const char *, TimelineKeyAction> actions[]{
					{"Quantize", TimelineKeyAction::Quantize},
					{"Align left", TimelineKeyAction::AlignLeft},
					{"Align center", TimelineKeyAction::AlignCenter},
					{"Align right", TimelineKeyAction::AlignRight},
					{"Distribute", TimelineKeyAction::Distribute},
					{"Reverse", TimelineKeyAction::Reverse}
				};
				ImGui::BeginDisabled(editor.Selection.empty() || Dragging || Boxing || editor.Active);
				for (const auto &[label, action] : actions)
					if (ImGui::MenuItem(label)) requestedAction = action;
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}
			ImGui::TextUnformatted("Shift: select. Ctrl+Alt: scale. Alt: copy. Wheel: zoom. Middle: pan.");
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const ImVec2 size{std::max(260.f, ImGui::GetContentRegionAvail().x), 200};
			ImGui::InvisibleButton(
				"##dopesheet",
				size,
				ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
					ImGuiButtonFlags_MouseButtonRight
			);
			const bool hovered = ImGui::IsItemHovered();
			const auto &io = ImGui::GetIO();
			const bool keyboardAvailable = hovered && !Dragging && !Boxing && !editor.Active &&
										   !io.WantTextInput && !ImGui::IsAnyItemActive() &&
										   !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
										   !io.KeyShift && !io.KeyAlt && !io.KeySuper;
			const bool deleteRequested =
				keyboardAvailable && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
			if (keyboardAvailable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
				(void)SelectAll(document, editor, error);
			if (keyboardAvailable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
				(void)editor.Copy(document, error);
			const bool pasteRequested =
				keyboardAvailable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false);

			if (keyboardAvailable && !io.KeyCtrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
					requestedAction = TimelineKeyAction::Quantize;
				else if (ImGui::IsKeyPressed(ImGuiKey_A, false))
					requestedAction = TimelineKeyAction::AlignLeft;
				else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
					requestedAction = TimelineKeyAction::Distribute;
				else if (ImGui::IsKeyPressed(ImGuiKey_I, false))
					requestedAction = TimelineKeyAction::Reverse;
			}
			const float labels = 140, header = 24, rowHeight = 24;
			const double timelineX = start.x + labels;
			if (!Dragging && !Boxing && hovered && io.MouseWheel != 0 && io.MousePos.x < start.x + 140)
				PanY += io.MouseWheel * 24;
			if (!Dragging && !Boxing && hovered && io.MouseWheel != 0 && io.MousePos.x >= start.x + 140) {
				const double at = (io.MousePos.x - timelineX - PanX) / PixelsPerFrame;
				PixelsPerFrame = std::clamp(PixelsPerFrame * std::pow(1.2, io.MouseWheel), 2.0, 256.0);
				PanX = io.MousePos.x - timelineX - at * PixelsPerFrame;
			}
			if (!Dragging && !Boxing && ImGui::IsItemActive() &&
				ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
				PanX += io.MouseDelta.x;
				PanY += io.MouseDelta.y;
			}
			PanX = std::clamp(
				PanX,
				-double(Limits::MaximumTick) * PixelsPerFrame,
				double(Limits::MaximumTick) * PixelsPerFrame
			);
			PanY = std::clamp(PanY, -double(Tracks.size()) * rowHeight, 0.0);
			auto *draw = ImGui::GetWindowDrawList();
			draw->AddRectFilled(start, {start.x + size.x, start.y + size.y}, IM_COL32(0, 0, 0, 255));
			draw->PushClipRect(start, {start.x + size.x, start.y + size.y}, true);
			for (size_t row = 0; row < Tracks.size(); ++row) {
				const float y = start.y + header + (row + .5f) * rowHeight + float(PanY);
				if (y < start.y + header || y > start.y + size.y) continue;
				const std::string label = Tracks[row].NodeId + "." + Tracks[row].Port;
				draw->PushClipRect({start.x, start.y + header}, {float(timelineX), start.y + size.y}, true);
				draw->AddText({start.x + 4, y - ImGui::GetFontSize() * .5f}, IM_COL32_WHITE, label.c_str());
				if (hovered && io.MousePos.x < timelineX && std::abs(io.MousePos.y - y) < rowHeight * .5f &&
					ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					(void)FocusTrack(Tracks[row], editor, error);
				draw->PopClipRect();
				draw->AddLine(
					{float(timelineX), y + rowHeight * .5f},
					{start.x + size.x, y + rowHeight * .5f},
					IM_COL32(255, 255, 255, 48)
				);
			}
			const double leftFrame = -PanX / PixelsPerFrame - 1;
			const double rightFrame = (start.x + size.x - timelineX - PanX) / PixelsPerFrame - 1;
			double interval = 1;
			while (interval * PixelsPerFrame < 48)
				interval *= 2;
			for (double frame = std::ceil(leftFrame / interval) * interval; frame <= rightFrame;
				 frame += interval) {
				const float x = float(timelineX + PanX + (frame + 1) * PixelsPerFrame);
				char label[32];
				std::snprintf(label, sizeof(label), "%.0f", frame);
				draw->AddText({x + 2, start.y + 2}, IM_COL32_WHITE, label);
				draw->AddLine({x, start.y + header}, {x, start.y + size.y}, IM_COL32(255, 255, 255, 32));
			}
			const float cursorX = float(timelineX + PanX + (FrameTimeToReal(cursor) + 1) * PixelsPerFrame);
			draw->AddLine(
				{cursorX, start.y + header}, {cursorX, start.y + size.y}, IM_COL32(255, 255, 255, 150)
			);
			std::optional<size_t> hit;
			for (auto &marker : Markers) {
				if (marker.Key >= document.Keyframes.size()) continue;
				const auto &key = document.Keyframes[marker.Key];
				FrameTime time = GetFrameTime(key);
				if (Dragging && !Copying)
					for (size_t index = 0; index < Originals.size(); ++index)
						if (key.NodeId == Originals[index].NodeId && key.Port == Originals[index].Port &&
							GetFrameTime(key) == GetFrameTime(Originals[index]))
							time = Destinations[index];
				marker.Position = {
					float(timelineX + PanX + (FrameTimeToReal(time) + 1) * PixelsPerFrame),
					start.y + header + (float(marker.Row) + .5f) * rowHeight + float(PanY)
				};
				if (marker.Position.x < timelineX || marker.Position.y < start.y + header ||
					marker.Position.y > start.y + size.y)
					continue;
				const auto colour = editor.Selected(key) ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 160);
				draw->AddQuad(
					{marker.Position.x, marker.Position.y - 6},
					{marker.Position.x + 6, marker.Position.y},
					{marker.Position.x, marker.Position.y + 6},
					{marker.Position.x - 6, marker.Position.y},
					colour
				);
				if (hovered && std::abs(io.MousePos.x - marker.Position.x) <= 8 &&
					std::abs(io.MousePos.y - marker.Position.y) <= 8)
					hit = marker.Key;
			}
			if (Dragging && Copying)
				for (size_t index = 0; index < Originals.size(); ++index) {
					const auto track = std::find_if(Tracks.begin(), Tracks.end(), [&](const auto &entry) {
						return entry.NodeId == Originals[index].NodeId && entry.Port == Originals[index].Port;
					});
					if (track == Tracks.end()) continue;
					const ImVec2 point{
						float(timelineX + PanX + (FrameTimeToReal(Destinations[index]) + 1) * PixelsPerFrame),
						start.y + header + (float(track - Tracks.begin()) + .5f) * rowHeight + float(PanY)
					};
					draw->AddQuad(
						{point.x, point.y - 6},
						{point.x + 6, point.y},
						{point.x, point.y + 6},
						{point.x - 6, point.y},
						IM_COL32_WHITE,
						2
					);
				}
			if (keyboardAvailable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
				!editor.Selection.empty()) {
				const auto found =
					std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
						return TimelineKeyEditor::Identity(key) == editor.Selection.front();
					});
				FrameTime mouse;
				if (found != document.Keyframes.end() &&
					SplitFrameTime((io.MousePos.x - timelineX - PanX) / PixelsPerFrame, mouse, true) &&
					ShiftFrameTime(mouse, {1, 0, false}, {}, mouse, false) &&
					Begin(
						document, editor, size_t(found - document.Keyframes.begin()), false, true, error, true
					)) {
					KeyboardCopy = true;
					MouseAnchor = mouse;
				}
			}
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !hit && !Dragging)
				editor.Selection.clear();
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !Dragging) {
				if (hit) {
					const auto &key = document.Keyframes[*hit];
					const auto identity = TimelineKeyEditor::Identity(key);
					if (io.KeyShift) {
						const auto found =
							std::find(editor.Selection.begin(), editor.Selection.end(), identity);
						if (found == editor.Selection.end())
							editor.Selection.push_back(identity);
						else
							editor.Selection.erase(found);
					} else {
						const bool scale =
							io.KeyCtrl && io.KeyAlt && !io.KeySuper && editor.Selection.size() > 1;
						const bool copy = io.KeyAlt && !io.KeyCtrl && !io.KeySuper;
						if (!io.KeyCtrl || scale) (void)Begin(document, editor, *hit, scale, copy, error);
					}
				} else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && io.MousePos.x >= timelineX &&
						   io.MousePos.x < start.x + size.x && io.MousePos.y >= start.y + header &&
						   io.MousePos.y < start.y + size.y) {
					const Marker *left = nullptr, *right = nullptr;
					for (const auto &marker : Markers) {
						if (marker.Key >= document.Keyframes.size() ||
							std::abs(io.MousePos.y - marker.Position.y) > 8)
							continue;
						if (marker.Position.x < io.MousePos.x &&
							(!left || marker.Position.x > left->Position.x))
							left = &marker;
						if (marker.Position.x > io.MousePos.x &&
							(!right || marker.Position.x < right->Position.x))
							right = &marker;
					}
					if (left && right && left->Row == right->Row && io.MousePos.x > left->Position.x + 8 &&
						io.MousePos.x < right->Position.x - 8) {
						editor.Selection = {
							TimelineKeyEditor::Identity(document.Keyframes[left->Key]),
							TimelineKeyEditor::Identity(document.Keyframes[right->Key])
						};
					} else if (io.MousePos.x >= timelineX && io.MousePos.y >= start.y + header) {
						if (!io.KeyShift) editor.Selection.clear();
						BoxSelection = std::move(editor.Selection);
						BoxStart = BoxEnd = io.MousePos;
						Boxing = true;
					}
				} else if (io.MousePos.x >= timelineX && io.MousePos.y >= start.y + header) {
					if (!io.KeyShift) editor.Selection.clear();
					BoxSelection = std::move(editor.Selection);
					BoxStart = BoxEnd = io.MousePos;
					Boxing = true;
				}
			}
			if (Boxing && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				editor.Selection = std::move(BoxSelection);
				Cancel();
			}
			if (Boxing) {
				BoxEnd = io.MousePos;
				const ImVec2 a{std::min(BoxStart.x, BoxEnd.x), std::min(BoxStart.y, BoxEnd.y)},
					b{std::max(BoxStart.x, BoxEnd.x), std::max(BoxStart.y, BoxEnd.y)};
				(void)UpdateBox(document, editor, a, b, error);
				draw->AddRect(a, b, IM_COL32_WHITE);
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
					Boxing = false;
					std::vector<ImageGraphKeyframeIdentity>().swap(BoxSelection);
				}
			}
			if (Dragging) {
				FrameTime endpoint;
				const double mouseFrame = (io.MousePos.x - timelineX - PanX) / PixelsPerFrame;
				if (SplitFrameTime(mouseFrame, endpoint, true) &&
					ShiftFrameTime(endpoint, {1, 0, false}, {}, endpoint) &&
					(!KeyboardCopy || ShiftFrameTime(endpoint, MouseAnchor, Anchor, endpoint)))
					(void)Update(endpoint, error);
				else {
					TargetsValid = false;
					error = {Status::InvalidValue, {}, {}, "key gesture exceeds the authored frame range"};
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Escape))
					Cancel();
				else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
					if (!apply()) Cancel();
				}
			}
			if (pasteRequested && paste && FocusedTrack && editor.Begin(document, true, cursor, error)) {
				const auto found = std::find_if(Tracks.begin(), Tracks.end(), [&](const auto &track) {
					return track.NodeId == FocusedTrack->NodeId && track.Port == FocusedTrack->Port;
				});
				if (found != Tracks.end()) {
					editor.TargetNode = FocusedTrack->NodeId;
					editor.TargetPort = FocusedTrack->Port;
					if (!paste()) editor.Cancel();
				} else
					editor.Cancel();
			}
			if (requestedAction && BeginAction(document, editor, *requestedAction, error)) {
				if (!apply()) Cancel();
			}
			if (deleteRequested && !Dragging && !Boxing && BeginDeletion(document, editor, error)) {
				if (!apply()) Cancel();
			}
			draw->PopClipRect();
		}
	};
}
