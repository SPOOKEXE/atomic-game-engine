#pragma once

#include "ImageGraphRegionBounds.hpp"

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <limits>
#include <new>
#include <numeric>

namespace studio {
	struct TimelineRegions {
		std::optional<size_t> Selected, DragTarget;
		uint64_t Revision = std::numeric_limits<uint64_t>::max();
		std::optional<engine::imagegraph::AnimationRegion> Original;
		std::vector<char> Label;
		double Start = 0, End = 0;
		float Color[3]{1, 1, 1};
		bool Editing = false, Dragging = false, DragEnd = false;
		bool NameEdited = false, ColorEdited = false, StartEdited = false, EndEdited = false;
		float DragMouse = 0;
		engine::imagegraph::FrameTime DragValue;

		void Bind(const engine::imagegraph::Document &document, ImageGraphPlayback &playback) const {
			playback.SelectedRegion.reset();
			if (Selected && document.Project && *Selected < document.Project->AnimationRegions.size()) {
				const auto &region = document.Project->AnimationRegions[*Selected];
				playback.SelectedRegion = std::pair{region.Start, region.End};
			}
		}
		void CancelEdit() {
			Original.reset();
			std::vector<char>().swap(Label);
			Editing = Dragging = false;
			DragTarget.reset();
		}
		bool Synchronize(
			const engine::imagegraph::Document &document, uint64_t revision, ImageGraphPlayback &playback
		) {
			if (Revision == revision) return false;
			const bool changed = playback.SelectedRegion.has_value();
			Selected.reset();
			CancelEdit();
			Revision = revision;
			Bind(document, playback);
			return changed;
		}
		bool BeginEdit(
			const engine::imagegraph::Document &document,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) {
			using namespace engine::imagegraph;
			if (!Selected || !document.Project || *Selected >= document.Project->AnimationRegions.size())
				return false;
			const auto held = DocumentRetainedPayloadBytes(document);
			const auto &region = document.Project->AnimationRegions[*Selected];
			const uint64_t names = region.Label.capacity() + region.SourceRegionId.capacity() + 2;
			const uint64_t scratch = Label.capacity() + Limits::MaximumTextBytes + 1 +
									 sizeof(AnimationRegion) + names +
									 (Original ? sizeof(AnimationRegion) + Original->Label.capacity() +
													 Original->SourceRegionId.capacity() + 2
											   : 0);
			if (!held || *held > maximumBytes || scratch > maximumBytes - *held ||
				region.Label.size() > Limits::MaximumTextBytes) {
				error = {
					Status::LimitExceeded, {}, "regions", "region settings exceed the editor payload budget"
				};
				return false;
			}
			try {
				// A fixed admitted buffer avoids allocating inside ImGui's resize callback.
				std::vector<char> candidate(Limits::MaximumTextBytes + 1);
				std::memcpy(candidate.data(), region.Label.data(), region.Label.size());
				AnimationRegion original = region;
				const uint64_t oldSettings =
					Label.capacity() + (Original ? sizeof(AnimationRegion) + Original->Label.capacity() +
													   Original->SourceRegionId.capacity() + 2
												 : 0);
				if (oldSettings + candidate.capacity() + original.Label.capacity() +
						original.SourceRegionId.capacity() + sizeof(AnimationRegion) + 2 >
					maximumBytes - *held)
					return false;
				Label = std::move(candidate);
				Original = std::move(original);
				Start = double(FrameTimeToReal(region.Start));
				End = double(FrameTimeToReal(region.End));
				Color[0] = region.Color.Red / 255.f;
				Color[1] = region.Color.Green / 255.f;
				Color[2] = region.Color.Blue / 255.f;
				NameEdited = ColorEdited = StartEdited = EndEdited = false;
				Editing = true;
				return true;
			} catch (const std::bad_alloc &) {
				error = {Status::LimitExceeded, {}, "regions", "region settings allocation failed"};
				return false;
			}
		}

	  private:
		// Only Commit calls this with the host-owned staged document. Refusal discards that copy.
		// The selected row follows the exact permutation, never a search by mutable fields.
		static bool Stage(
			engine::imagegraph::Document &document,
			std::optional<size_t> target,
			const std::optional<engine::imagegraph::AnimationRegion> &replacement,
			bool clear,
			std::optional<size_t> selected,
			std::optional<size_t> &nextSelection,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) {
			using namespace engine::imagegraph;
			if (!document.Project && clear) {
				nextSelection.reset();
				return true;
			}
			if (!document.Project) document.Project.emplace();
			auto &regions = document.Project->AnimationRegions;
			if ((target && *target >= regions.size()) ||
				(!target && replacement && regions.size() >= Limits::MaximumAnimationRegions))
				return false;
			const auto retained = DocumentRetainedPayloadBytes(document);
			const uint64_t count = regions.size() + (!target && replacement ? 1 : 0);
			const uint64_t scratch =
				count * (sizeof(AnimationRegion) + sizeof(size_t) * 2) +
				((!target && replacement && count > regions.capacity()) ? count * sizeof(AnimationRegion)
																		: 0) +
				(replacement ? sizeof(AnimationRegion) + replacement->Label.capacity() * 2 +
								   replacement->SourceRegionId.capacity() * 2 + 4
							 : 0);
			if (!retained || *retained > maximumBytes || scratch > maximumBytes - *retained) {
				error = {
					Status::LimitExceeded, {}, "regions", "region edit exceeds the editor payload budget"
				};
				return false;
			}
			if (clear) {
				regions.clear();
				document.FormatVersion = 9;
				nextSelection.reset();
				return true;
			}
			if (replacement) {
				if (replacement->Label.size() > Limits::MaximumTextBytes ||
					!ValidFrameTime(replacement->Start) || !ValidFrameTime(replacement->End)) {
					error = {Status::InvalidValue, {}, "regions", "region fields exceed the authored bounds"};
					return false;
				}
				if (target)
					regions[*target] = *replacement;
				else {
					regions.reserve(count);
					regions.push_back(*replacement);
				}
			} else if (target) {
				regions.erase(regions.begin() + *target);
				if (selected == target)
					selected.reset();
				else if (selected && *selected > *target)
					--*selected;
			}
			for (auto &region : regions) {
				if (CompareFrameTime(region.End, region.Start) < 0) std::swap(region.Start, region.End);
			}
			std::vector<size_t> order(regions.size());
			std::iota(order.begin(), order.end(), size_t{0});
			std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				return regions[a].Start == regions[b].Start
						   ? CompareFrameTime(regions[a].End, regions[b].End) < 0
						   : CompareFrameTime(regions[a].Start, regions[b].Start) < 0;
			});
			std::vector<AnimationRegion> sorted;
			sorted.reserve(regions.size());
			const auto currentBytes = DocumentRetainedPayloadBytes(document);
			const uint64_t sortingBytes =
				sorted.capacity() * sizeof(AnimationRegion) + order.capacity() * sizeof(size_t) +
				order.size() * sizeof(size_t) +
				(replacement ? sizeof(AnimationRegion) + replacement->Label.capacity() +
								   replacement->SourceRegionId.capacity() + 2
							 : 0);
			if (!currentBytes || *currentBytes > maximumBytes ||
				sortingBytes > maximumBytes - *currentBytes) {
				error = {
					Status::LimitExceeded, {}, "regions", "region sorting exceeds the editor payload budget"
				};
				return false;
			}
			std::optional<size_t> next;
			for (size_t index : order) {
				if (selected == index) next = sorted.size();
				sorted.push_back(std::move(regions[index]));
			}
			regions = std::move(sorted);
			document.FormatVersion = 9;
			if (!ValidProjectAnimationRegions(*document.Project)) return false;
			nextSelection = next;
			error = {};
			return true;
		}

	  public:
		template <class Apply>
		bool Commit(
			const engine::imagegraph::Document &document,
			uint64_t &revision,
			ImageGraphPlayback &playback,
			std::optional<size_t> target,
			const std::optional<engine::imagegraph::AnimationRegion> &replacement,
			bool clear,
			const Apply &apply,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			const auto bytes = DocumentRetainedPayloadBytes(document);
			const uint64_t scratch = Label.capacity() + (Original ? Original->Label.capacity() +
																		Original->SourceRegionId.capacity() +
																		sizeof(AnimationRegion) + 2
																  : 0);
			if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 2 ||
				scratch > Limits::MaximumEvaluationBytes - *bytes * 2) {
				error = {
					Status::LimitExceeded,
					{},
					"regions",
					"region transaction exceeds the editor payload budget"
				};
				return false;
			}
			std::optional<size_t> next = Selected;
			if (!apply([&](Document &staged) {
					if (Original &&
						(!target || !staged.Project || *target >= staged.Project->AnimationRegions.size() ||
						 staged.Project->AnimationRegions[*target] != *Original)) {
						error = {Status::InvalidValue, {}, "regions", "region settings are stale"};
						return false;
					}
					if (!target && replacement && !staged.Timeline) {
						staged.Timeline = detail::RegionPlaybackTimeline(playback);
						staged.Timeline->SourceBounds = SourceAuthoringFrameBounds{};
					}
					return Stage(
						staged,
						target,
						replacement,
						clear,
						Selected,
						next,
						error,
						Limits::MaximumEvaluationBytes - *bytes - scratch
					);
				}))
				return false;
			Selected = next;
			Revision = revision;
			if (document.Timeline) playback.SourceBounds = document.Timeline->SourceBounds;
			CancelEdit();
			Bind(document, playback);
			return true;
		}
		template <class Apply>
		bool ClearSourceRange(
			const engine::imagegraph::Document &document,
			uint64_t &revision,
			ImageGraphPlayback &playback,
			const Apply &apply,
			engine::imagegraph::Diagnostic &error
		) {
			using namespace engine::imagegraph;
			const auto held = DocumentRetainedPayloadBytes(document);
			if (!held || *held > Limits::MaximumEvaluationBytes / 2) {
				error = {
					Status::LimitExceeded, {}, "timeline", "source range reset exceeds the editor budget"
				};
				return false;
			}
			auto timeline = detail::RegionPlaybackTimeline(playback);
			timeline.SourceBounds = SourceAuthoringFrameBounds{};
			if (ProjectSourceTimelineWindow(timeline, error) != Status::Ok) return false;
			if (!apply([&](Document &staged) { return SetImageGraphTimeline(staged, timeline, error); }))
				return false;
			Revision = revision;
			playback.SourceBounds = timeline.SourceBounds;
			playback.StartTick = timeline.First;
			playback.EndTick = timeline.Last;
			Bind(document, playback);
			return true;
		}
		template <class Apply>
		bool Draw(
			const engine::imagegraph::Document &document,
			uint64_t &revision,
			ImageGraphPlayback &playback,
			double pixelsPerFrame,
			double panX,
			engine::imagegraph::Diagnostic &error,
			const Apply &apply
		) {
			using namespace engine::imagegraph;
			bool changed = Synchronize(document, revision, playback);
			if (ImGui::Button("Create region")) {
				AnimationRegion region;
				if (SplitFrameTime(double(FrameTimeToReal(GetImageGraphFrame(playback))) + 1, region.Start)) {
					region.End = {playback.TotalFrames, 0, false};
					changed |= Commit(document, revision, playback, {}, region, false, apply, error);
				}
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!Selected);
			if (ImGui::Button("Region settings") && BeginEdit(document, error))
				ImGui::OpenPopup("##region-settings");
			ImGui::SameLine();
			if (ImGui::Button("Delete region"))
				changed |= Commit(document, revision, playback, Selected, {}, false, apply, error);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!document.Project || document.Project->AnimationRegions.empty());
			if (ImGui::Button("Clear regions"))
				changed |= Commit(document, revision, playback, {}, {}, true, apply, error);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Clear source range"))
				changed |= ClearSourceRange(document, revision, playback, apply, error);
			const bool settingsOpen = ImGui::BeginPopup("##region-settings");
			if (settingsOpen) {
				if (Editing && Original) {
					NameEdited |= ImGui::InputText("Name", Label.data(), Label.size());
					ColorEdited |= ImGui::ColorEdit3("Color", Color, ImGuiColorEditFlags_NoInputs);
					const bool startEdited = ImGui::InputDouble("Frame start", &Start, 1, 10, "%.17g");
					const bool endEdited = ImGui::InputDouble("Frame end", &End, 1, 10, "%.17g");
					StartEdited |= startEdited;
					EndEdited |= endEdited;
					FrameTime rounded;
					if (startEdited && SplitFrameTime(Start, rounded, true))
						Start = double(FrameTimeToReal(rounded));
					if (endEdited && SplitFrameTime(End, rounded, true))
						End = double(FrameTimeToReal(rounded));
					if (ImGui::Button("Apply region")) {
						AnimationRegion candidate = *Original;
						if (NameEdited) candidate.Label.assign(Label.data());
						if (ColorEdited)
							candidate.Color = {
								uint8_t(std::clamp(Color[0], 0.f, 1.f) * 255 + .5f),
								uint8_t(std::clamp(Color[1], 0.f, 1.f) * 255 + .5f),
								uint8_t(std::clamp(Color[2], 0.f, 1.f) * 255 + .5f),
								255
							};
						const bool valid = (!StartEdited || SplitFrameTime(Start, candidate.Start)) &&
										   (!EndEdited || SplitFrameTime(End, candidate.End));
						if (!valid)
							error = {Status::InvalidValue, {}, "regions", "region frame fields are invalid"};
						if (valid &&
							Commit(document, revision, playback, Selected, candidate, false, apply, error)) {
							changed = true;
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel region")) {
						CancelEdit();
						ImGui::CloseCurrentPopup();
					}
				} else {
					// Revision synchronization retired this popup's original region.
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			} else if (Editing && !ImGui::IsPopupOpen("##region-settings"))
				CancelEdit();
			if (!document.Project || document.Project->AnimationRegions.empty()) return changed;
			const auto &regions = document.Project->AnimationRegions;
			const auto origin = ImGui::GetCursorScreenPos();
			const ImVec2 size{
				std::max(260.f, ImGui::GetContentRegionAvail().x), 24.f * float(regions.size())
			};
			ImGui::InvisibleButton("##animation-regions", size);
			const bool hovered = ImGui::IsItemHovered();
			const auto &io = ImGui::GetIO();
			auto *draw = ImGui::GetWindowDrawList();
			draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
			for (size_t i = 0; i < regions.size(); ++i) {
				const auto &region = regions[i];
				const float y = origin.y + (float(i) + .5f) * 24;
				FrameTime drawnStart = region.Start, drawnEnd = region.End;
				if (Dragging && DragTarget == i) {
					FrameTime delta, endpoint;
					if (SplitFrameTime((io.MousePos.x - DragMouse) / pixelsPerFrame, delta, true) &&
						SplitFrameTime(double(FrameTimeToReal(DragValue) + FrameTimeToReal(delta)), endpoint))
						(DragEnd ? drawnEnd : drawnStart) = endpoint;
				}
				const float x0 = float(origin.x + 140 + panX + FrameTimeToReal(drawnStart) * pixelsPerFrame);
				const float x1 = float(origin.x + 140 + panX + FrameTimeToReal(drawnEnd) * pixelsPerFrame);
				const bool rowHover = hovered && std::abs(io.MousePos.y - y) < 12;
				const auto col = IM_COL32(
					region.Color.Red, region.Color.Green, region.Color.Blue, Selected == i ? 255 : 128
				);
				draw->AddText(
					{origin.x + 4, y - ImGui::GetFontSize() * .5f}, IM_COL32_WHITE, region.Label.c_str()
				);
				draw->AddRectFilled({std::min(x0, x1), y - 5}, {std::max(x0, x1), y + 5}, col);
				draw->AddCircleFilled({x0, y}, 4, col);
				draw->AddCircleFilled({x1, y}, 4, col);
				if (!Dragging && !Editing && rowHover &&
					!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
					ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					const bool first = std::abs(io.MousePos.x - x0) <= 7,
							   last = std::abs(io.MousePos.x - x1) <= 7;
					if ((first || last) && BeginDrag(document, i, last && !first, io.MousePos.x, error)) {
					} else if (io.MousePos.x < origin.x + 140 ||
							   (io.MousePos.x >= x0 && io.MousePos.x <= x1)) {
						Selected = Selected == i ? std::nullopt : std::optional<size_t>(i);
						Bind(document, playback);
						changed = true;
					}
				}
			}
			if (Dragging && Original && DragTarget) {
				FrameTime delta, endpoint;
				if (SplitFrameTime((io.MousePos.x - DragMouse) / pixelsPerFrame, delta, true) &&
					SplitFrameTime(double(FrameTimeToReal(DragValue) + FrameTimeToReal(delta)), endpoint)) {
					if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
						AnimationRegion candidate = *Original;
						(DragEnd ? candidate.End : candidate.Start) = endpoint;
						changed |= Commit(
							document,
							revision,
							playback,
							DragTarget,
							candidate.Start == candidate.End ? std::optional<AnimationRegion>{} : candidate,
							false,
							apply,
							error
						);
						CancelEdit();
					}
				}
			}
			draw->PopClipRect();
			return changed;
		}
		bool BeginDrag(
			const engine::imagegraph::Document &document,
			size_t row,
			bool end,
			float mouse,
			engine::imagegraph::Diagnostic &error
		) {
			const auto old = Selected;
			Selected = row;
			const bool captured = BeginEdit(document, error);
			Selected = old;
			if (!captured) return false;
			DragTarget = row;
			Editing = false;
			Dragging = true;
			DragEnd = end;
			DragMouse = mouse;
			DragValue = end ? Original->End : Original->Start;
			return true;
		}
	};
}
