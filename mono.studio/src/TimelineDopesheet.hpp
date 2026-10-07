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
#include <tuple>

namespace studio {
	// The display cache contains identities and marker geometry, never authored values.
	// Gestures pin bounded full keys; each accepted edit owns one host transaction.
	struct TimelineDopesheet {
		struct Track {
			std::string NodeId, Port;
			int8_t Axis = -1;
		};
		struct Marker {
			size_t Row = 0;
			ImVec2 Position;
			engine::imagegraph::FrameTime Time;
		};
		std::vector<Track> Tracks;
		std::optional<Track> FocusedTrack;
		std::vector<Marker> Markers;
		std::vector<engine::imagegraph::Keyframe> Originals;
		std::vector<int8_t> OriginalAxes;
		std::vector<engine::imagegraph::FrameTime> Destinations;
		std::vector<ImageGraphKeyframeIdentity> BoxSelection, PreparedSelection;
		bool Prepared = false;
		uint64_t Revision = std::numeric_limits<uint64_t>::max();
		uint64_t OriginalObservationRevision = 0;
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
				OriginalAxes.capacity() + Destinations.capacity() * sizeof(FrameTime) +
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
			if (FocusedTrack && FocusedTrack->NodeId == track.NodeId && FocusedTrack->Port == track.Port &&
				FocusedTrack->Axis == track.Axis)
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
				Track candidate{track.NodeId, track.Port, track.Axis};
				const uint64_t actual =
					sizeof(Track) + candidate.NodeId.capacity() + candidate.Port.capacity() + 2;
				if (actual > *budget - *held) return refuse();
				FocusedTrack = std::move(candidate);
				return true;
			} catch (const std::bad_alloc &) {
				return refuse();
			}
		}
		ImageGraphKeyframeIdentity MarkerIdentity(const Marker &marker) const {
			const auto &track = Tracks[marker.Row];
			return {track.NodeId, track.Port, marker.Time, track.Axis};
		}
		bool MatchesMarker(const Marker &marker, const ImageGraphKeyframeIdentity &identity) const {
			const auto &track = Tracks[marker.Row];
			return identity.NodeId == track.NodeId && identity.Port == track.Port &&
				   identity.Time == marker.Time && identity.Axis == track.Axis;
		}
		// grug keep clocks and names in the display cache, never copies of authored key values.
		template <class Visit>
		static bool VisitMarkers(
			const engine::imagegraph::Document &document,
			std::span<const engine::imagegraph::SourceAxisObservation> axes,
			const Visit &visit
		) {
			using namespace engine::imagegraph;
			uint64_t work = 0;
			for (const auto &key : document.Keyframes)
				if (++work > 64'000'000 || !visit(key, key.NodeId, key.Port, int8_t{-1})) return false;
			for (const auto &view : axes)
				for (int8_t axis = 0; axis < 2; ++axis)
					for (const auto &key : view.Storage->Axes[size_t(axis)].Keys)
						if (++work > 64'000'000 || !visit(key, view.NodeId, view.Port, axis)) return false;
			return true;
		}
		bool UpdateBox(
			const engine::imagegraph::Document &,
			TimelineKeyEditor &editor,
			const ImVec2 &a,
			const ImVec2 &b,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			const auto budget = editor.Remaining(true, true), retained = CacheBytes();
			if (!budget || !retained || *retained > *budget) return false;
			uint64_t remaining = *budget - *retained;
			uint64_t workRemaining = 64'000'000;
			if (BoxSelection.size() > Limits::MaximumKeyframes) return false;
			for (const auto &marker : Markers) {
				if (marker.Row >= Tracks.size()) return false;
				const auto &track = Tracks[marker.Row];
				const uint64_t names = 1 + track.NodeId.size() + track.Port.size();
				const uint64_t scans = 2 * BoxSelection.size();
				if (scans && names > workRemaining / scans) {
					error = {Status::LimitExceeded, {}, {}, "key box selection exceeds the work bound"};
					return false;
				}
				workRemaining -= names * scans;
			}
			const auto inside = [&](const Marker &marker) {
				return marker.Position.x >= a.x && marker.Position.x <= b.x && marker.Position.y >= a.y &&
					   marker.Position.y <= b.y;
			};
			const auto saved = [&](const Marker &marker) {
				const auto &track = Tracks[marker.Row];
				return std::any_of(BoxSelection.begin(), BoxSelection.end(), [&](const auto &id) {
					return id.Axis == track.Axis && id.NodeId == track.NodeId && id.Port == track.Port &&
						   id.Time == marker.Time;
				});
			};
			size_t count = BoxSelection.size();
			for (const auto &id : BoxSelection) {
				const uint64_t bytes = sizeof(ImageGraphKeyframeIdentity) + id.NodeId.size() + id.Port.size();
				if (bytes > remaining) return false;
				remaining -= bytes;
			}
			for (const auto &marker : Markers) {
				if (marker.Row >= Tracks.size()) return false;
				if (!inside(marker) || saved(marker)) continue;
				const auto &track = Tracks[marker.Row];
				const uint64_t bytes =
					sizeof(ImageGraphKeyframeIdentity) + track.NodeId.size() + track.Port.size();
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
			if ((candidate.capacity() - count) * sizeof(ImageGraphKeyframeIdentity) > remaining) return false;
			candidate.insert(candidate.end(), BoxSelection.begin(), BoxSelection.end());
			for (const auto &marker : Markers)
				if (inside(marker) && !saved(marker)) candidate.push_back(MarkerIdentity(marker));
			editor.Selection = std::move(candidate);
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "key selection allocation failed"};
			return false;
		}
		bool Rebuild(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			const TimelineKeyEditor &editor,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			if (Revision == revision) return true;
			const auto budget = editor.Remaining(true, true), retained = CacheBytes();
			uint64_t remaining = budget.value_or(0), workRemaining = 64'000'000;
			const auto refuse = [&] {
				error = {Status::LimitExceeded, {}, {}, "timeline display exceeds payload or work bounds"};
				return false;
			};
			if (!retained || *retained > remaining) return refuse();
			remaining -= *retained;
			std::vector<SourceAxisObservation> axes;
			if (ObserveSourceKeyframeAxes(document, axes, error, remaining) != Status::Ok) return false;
			const auto resident = DocumentRetainedPayloadBytes(document);
			const uint64_t views = axes.capacity() * sizeof(SourceAxisObservation);
			if (!resident || *resident > remaining || views > remaining - *resident) return refuse();
			remaining -= *resident + views;
			using RowKey = std::tuple<std::string_view, std::string_view, int8_t>;
			constexpr uint64_t perKey =
				sizeof(Marker) + sizeof(Track) + sizeof(std::pair<const RowKey, size_t>) + 4 * sizeof(void *);
			size_t count = 0;
			if (!VisitMarkers(
					document,
					axes,
					[&](const Keyframe &key, std::string_view node, std::string_view port, int8_t) {
						const uint64_t names = node.size() + port.size() + 2;
						if (count >= Limits::MaximumKeyframes || !ValidFrameTime(GetFrameTime(key)) ||
							node.size() > Limits::MaximumTextBytes ||
							port.size() > Limits::MaximumTextBytes || perKey > remaining ||
							names > remaining - perKey || names > workRemaining / 128)
							return false;
						remaining -= perKey + names;
						workRemaining -= 128 * names;
						++count;
						return true;
					}
				))
				return refuse();
			std::vector<Track> tracks;
			std::vector<Marker> markers;
			tracks.reserve(count);
			markers.reserve(count);
			const uint64_t spare =
				(tracks.capacity() - count) * sizeof(Track) + (markers.capacity() - count) * sizeof(Marker);
			if (spare > remaining) return refuse();
			std::map<RowKey, size_t> rows;
			if (!VisitMarkers(
					document,
					axes,
					[&](const Keyframe &key, std::string_view node, std::string_view port, int8_t axis) {
						const auto [entry, inserted] = rows.emplace(RowKey{node, port, axis}, tracks.size());
						if (inserted) tracks.push_back({std::string(node), std::string(port), axis});
						markers.push_back({entry->second, {}, GetFrameTime(key)});
						return true;
					}
				))
				return refuse();
			Tracks = std::move(tracks);
			Markers = std::move(markers);
			Revision = revision;
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "timeline display allocation failed"};
			return false;
		}
		void Cancel() {
			Dragging = Scaling = Copying = Boxing = Deleting = Prepared = Transforming = KeyboardCopy = false;
			std::vector<engine::imagegraph::Keyframe>().swap(Originals);
			std::vector<int8_t>().swap(OriginalAxes);
			std::vector<engine::imagegraph::FrameTime>().swap(Destinations);
			std::vector<ImageGraphKeyframeIdentity>().swap(BoxSelection);
			std::vector<ImageGraphKeyframeIdentity>().swap(PreparedSelection);
		}
		bool SelectAll(
			const engine::imagegraph::Document &,
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
				if (marker.Row >= Tracks.size()) {
					error = {Status::InvalidValue, {}, {}, "key selection display is stale"};
					return false;
				}
				const auto &track = Tracks[marker.Row];
				const uint64_t bytes =
					sizeof(ImageGraphKeyframeIdentity) + track.NodeId.size() + track.Port.size();
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
				selected.push_back(MarkerIdentity(marker));
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
			if (!TimelineKeyEditor::CaptureSelection(
					document, editor.Selection, Originals, OriginalAxes, error, *budget - *held
				))
				return false;
			const auto captured = CacheBytes();
			if (!captured || *captured > *budget ||
				!PrepareTimelineKeyDestinations(
					Originals, action, Destinations, error, *budget - *captured, OriginalAxes
				)) {
				Cancel();
				return false;
			}
			OriginalObservationRevision = Revision;
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
			if (!TimelineKeyEditor::CaptureSelection(
					document, editor.Selection, Originals, OriginalAxes, error, *budget - *retained
				))
				return false;
			OriginalObservationRevision = Revision;
			Deleting = true;
			return true;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "key deletion capture allocation failed"
			};
			return false;
		}
		bool BeginIdentity(
			const engine::imagegraph::Document &document,
			TimelineKeyEditor &editor,
			const ImageGraphKeyframeIdentity &identity,
			bool scale,
			bool copy,
			engine::imagegraph::Diagnostic &error,
			bool keepCopySelection = false
		) try {
			using namespace engine::imagegraph;
			const FrameTime anchor = identity.Time;
			const bool replaceSelection =
				(copy && !keepCopySelection) ||
				std::find(editor.Selection.begin(), editor.Selection.end(), identity) ==
					editor.Selection.end();
			// Admit the gesture before replacing selection, including refused alias pins.
			const auto selection = replaceSelection
									   ? std::span<const ImageGraphKeyframeIdentity>{&identity, 1}
									   : std::span<const ImageGraphKeyframeIdentity>{editor.Selection};
			const auto budget = editor.Remaining(true, true);
			const auto retained = CacheBytes();
			const uint64_t clocks = selection.size() * sizeof(FrameTime) +
									(replaceSelection ? sizeof(ImageGraphKeyframeIdentity) +
															identity.NodeId.size() + identity.Port.size()
													  : 0);
			if (!budget || !retained || *retained > *budget || clocks > *budget - *retained) {
				error = {
					Status::LimitExceeded,
					identity.NodeId,
					identity.Port,
					"key gesture exceeds the timeline payload budget"
				};
				return false;
			}
			if (!TimelineKeyEditor::CaptureSelection(
					document, selection, Originals, OriginalAxes, error, *budget - *retained - clocks
				))
				return false;
			OriginalObservationRevision = Revision;
			Anchor = anchor;
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
			if (replaceSelection) {
				std::vector<ImageGraphKeyframeIdentity> candidate{identity};
				editor.Selection.swap(candidate);
			}
			Dragging = true;
			Scaling = scale;
			Copying = copy;
			TargetsValid = true;
			return true;
		} catch (const std::bad_alloc &) {
			Cancel();
			error = {
				engine::imagegraph::Status::LimitExceeded,
				identity.NodeId,
				identity.Port,
				"key gesture allocation failed"
			};
			return false;
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
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
			std::span<const engine::imagegraph::Keyframe> projectedPins = {}
		) {
			using namespace engine::imagegraph;
			Prepared = false;
			const auto originals =
				projectedPins.empty() ? std::span<const Keyframe>{Originals} : projectedPins;
			if (originals.size() != Originals.size() ||
				(!OriginalAxes.empty() && OriginalAxes.size() != originals.size()) ||
				std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) {
					return axis < -1 || axis > 1;
				})) {
				error = {Status::InvalidValue, {}, {}, "timeline keys have invalid component selectors"};
				return false;
			}
			const bool sourceKeys =
				document.SourceAnimators ||
				std::any_of(OriginalAxes.begin(), OriginalAxes.end(), [](int8_t axis) { return axis != -1; });

			const auto budget = editor.Remaining(true, true, maximumBytes);
			uint64_t remaining = budget.value_or(0);
			const auto retained = CacheBytes();
			if ((!Dragging && !Deleting && !Transforming) || (!Deleting && !TargetsValid) || !retained ||
				*retained > remaining)
				return false;
			if (Deleting && (originals.empty() || document.Keyframes.size() > Limits::MaximumKeyframes ||
							 originals.size() > Limits::MaximumKeyframes ||
							 uint64_t(originals.size()) * document.Keyframes.size() * 2 > 64'000'000)) {
				error = {Status::LimitExceeded, {}, {}, "key deletion exceeds the work bound"};
				return false;
			}
			if (!sourceKeys)
				for (const auto &original : originals)
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
			if (Deleting && sourceKeys) {
				remaining -= *retained;
				const uint64_t scratch = originals.size() * sizeof(SourceKeyframeEdit);
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
				edits.reserve(originals.size());
				for (size_t index = 0; index < originals.size(); ++index)
					edits.push_back(
						{&originals[index],
						 nullptr,
						 false,
						 OriginalAxes.empty() ? int8_t{-1} : OriginalAxes[index]}
					);
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
					return std::any_of(originals.begin(), originals.end(), [&](const auto &original) {
						return key.NodeId == original.NodeId && key.Port == original.Port &&
							   GetFrameTime(key) == GetFrameTime(original);
					});
				});
				std::vector<ImageGraphKeyframeIdentity>{}.swap(PreparedSelection);
				Prepared = true;
				return true;
			}
			remaining -= *retained;
			for (const auto &original : originals) {
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
			selection.reserve(originals.size());
			for (size_t index = 0; index < originals.size(); ++index) {
				ImageGraphKeyframeIdentity identity{
					originals[index].NodeId,
					originals[index].Port,
					!Transforming && Destinations[index].NegativeFrame ? FrameTime{} : Destinations[index],
					OriginalAxes.empty() ? int8_t{-1} : OriginalAxes[index]
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
					document, originals, Destinations, Copying, error, remaining, !Transforming, OriginalAxes
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
				const std::string label = Tracks[row].NodeId + "." + Tracks[row].Port +
										  (Tracks[row].Axis < 0	   ? ""
										   : Tracks[row].Axis == 0 ? ".x"
																   : ".y");
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
			for (size_t markerIndex = 0; markerIndex < Markers.size(); ++markerIndex) {
				auto &marker = Markers[markerIndex];
				if (marker.Row >= Tracks.size()) continue;
				const auto &track = Tracks[marker.Row];
				FrameTime time = marker.Time;
				if (Dragging && !Copying)
					for (size_t index = 0; index < Originals.size(); ++index)
						if ((OriginalAxes.empty() ? int8_t{-1} : OriginalAxes[index]) == track.Axis &&
							track.NodeId == Originals[index].NodeId && track.Port == Originals[index].Port &&
							marker.Time == GetFrameTime(Originals[index]))
							time = Destinations[index];
				marker.Position = {
					float(timelineX + PanX + (FrameTimeToReal(time) + 1) * PixelsPerFrame),
					start.y + header + (float(marker.Row) + .5f) * rowHeight + float(PanY)
				};
				if (marker.Position.x < timelineX || marker.Position.y < start.y + header ||
					marker.Position.y > start.y + size.y)
					continue;
				const auto colour = std::any_of(
										editor.Selection.begin(),
										editor.Selection.end(),
										[&](const auto &identity) { return MatchesMarker(marker, identity); }
									)
										? IM_COL32_WHITE
										: IM_COL32(255, 255, 255, 160);
				draw->AddQuad(
					{marker.Position.x, marker.Position.y - 6},
					{marker.Position.x + 6, marker.Position.y},
					{marker.Position.x, marker.Position.y + 6},
					{marker.Position.x - 6, marker.Position.y},
					colour
				);
				if (hovered && std::abs(io.MousePos.x - marker.Position.x) <= 8 &&
					std::abs(io.MousePos.y - marker.Position.y) <= 8)
					hit = markerIndex;
			}
			if (Dragging && Copying)
				for (size_t index = 0; index < Originals.size(); ++index) {
					const auto track = std::find_if(Tracks.begin(), Tracks.end(), [&](const auto &entry) {
						return entry.NodeId == Originals[index].NodeId &&
							   entry.Port == Originals[index].Port &&
							   entry.Axis == (OriginalAxes.empty() ? int8_t{-1} : OriginalAxes[index]);
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
				const auto found = std::find_if(Markers.begin(), Markers.end(), [&](const auto &marker) {
					return marker.Row < Tracks.size() && MatchesMarker(marker, editor.Selection.front());
				});
				FrameTime mouse;
				if (found != Markers.end() &&
					SplitFrameTime((io.MousePos.x - timelineX - PanX) / PixelsPerFrame, mouse, true) &&
					ShiftFrameTime(mouse, {1, 0, false}, {}, mouse, false) &&
					BeginIdentity(document, editor, MarkerIdentity(*found), false, true, error, true)) {
					KeyboardCopy = true;
					MouseAnchor = mouse;
				}
			}
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !hit && !Dragging)
				editor.Selection.clear();
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !Dragging) {
				if (hit) {
					const auto identity = MarkerIdentity(Markers[*hit]);
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
						if (!io.KeyCtrl || scale)
							(void)BeginIdentity(document, editor, identity, scale, copy, error);
					}
				} else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && io.MousePos.x >= timelineX &&
						   io.MousePos.x < start.x + size.x && io.MousePos.y >= start.y + header &&
						   io.MousePos.y < start.y + size.y) {
					const Marker *left = nullptr, *right = nullptr;
					for (const auto &marker : Markers) {
						if (marker.Row >= Tracks.size() || std::abs(io.MousePos.y - marker.Position.y) > 8)
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
						editor.Selection = {MarkerIdentity(*left), MarkerIdentity(*right)};
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
			if (pasteRequested && paste && FocusedTrack) {
				if (editor.Begin(document, true, cursor, error)) {
					const auto found = std::find_if(Tracks.begin(), Tracks.end(), [&](const auto &track) {
						return track.NodeId == FocusedTrack->NodeId && track.Port == FocusedTrack->Port &&
							   track.Axis == FocusedTrack->Axis;
					});
					if (found != Tracks.end()) {
						editor.TargetNode = FocusedTrack->NodeId;
						editor.TargetPort = FocusedTrack->Port;
						editor.TargetAxis = FocusedTrack->Axis;
						if (!paste()) editor.Cancel();
					} else
						editor.Cancel();
				}
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
