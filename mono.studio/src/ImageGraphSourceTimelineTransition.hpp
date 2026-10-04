#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>

#include <new>
#include <optional>
#include <stdexcept>
#include <studio/ImageGraph.hpp>
#include <utility>

namespace studio::detail {
	struct PreparedSourceTimelineStep {
		ImageGraphPlayback Playback;
		std::optional<engine::imagegraph::TimelineSettings> AuthoredTimeline;
		bool BoundsChanged = false;
	};

	inline bool PrepareSourceTimelineStep(
		const engine::imagegraph::Document &document,
		const ImageGraphPlayback &playback,
		PreparedSourceTimelineStep &prepared,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		prepared = {};
		prepared.Playback = playback;
		if (!playback.SourceBounds) {
			diagnostic = {};
			return true;
		}
		if (!document.Timeline || document.Timeline->SourceBounds != playback.SourceBounds) {
			diagnostic = {
				Status::InvalidValue, {}, "timeline", "source step no longer matches the authored timeline"
			};
			return false;
		}
		TimelineSettings timeline{
			playback.TotalFrames,
			playback.StartTick,
			playback.EndTick,
			playback.PingPong ? "pingpong"
			: playback.Loop	  ? "loop"
							  : "stop",
			playback.FramesPerSecond,
			playback.SourceBounds
		};
		const auto previousBounds = timeline.SourceBounds;
		if (NormalizeSourceTimelineBounds(timeline, diagnostic) != Status::Ok) return false;
		prepared.Playback.TotalFrames = timeline.Frames;
		prepared.Playback.StartTick = timeline.First;
		prepared.Playback.EndTick = timeline.Last;
		prepared.Playback.SourceBounds = timeline.SourceBounds;
		prepared.BoundsChanged = previousBounds != timeline.SourceBounds;
		if (prepared.BoundsChanged) prepared.AuthoredTimeline = std::move(timeline);
		diagnostic = {};
		return true;
	}

	inline bool CommitSourceTimelineStep(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphPlayback &playback,
		PreparedSourceTimelineStep prepared,
		bool &documentChanged,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		using namespace engine::imagegraph;
		documentChanged = false;
		if (!prepared.AuthoredTimeline) {
			// Ordinary frame ticks publish playback state without copying the authored graph.
			playback = prepared.Playback;
			diagnostic = {};
			return true;
		}
		const auto beforeBytes = DocumentRetainedPayloadBytes(document);
		// Reserve both retained document versions before cloning; history owns its own serialized-byte
		// admission.
		if (!beforeBytes || *beforeBytes >= maximumBytes || *beforeBytes > maximumBytes - *beforeBytes) {
			diagnostic = {
				Status::LimitExceeded, {}, "timeline", "source step exceeds the document overlap budget"
			};
			return false;
		}
		try {
			Document candidate = document;
			if (!SetImageGraphTimeline(candidate, *prepared.AuthoredTimeline, diagnostic)) return false;
			const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
			if (!candidateBytes || *candidateBytes > maximumBytes - *beforeBytes) {
				diagnostic = {
					Status::LimitExceeded, {}, "timeline", "source step exceeds the document overlap budget"
				};
				return false;
			}
			if (!history.TryRecord(document, candidate)) {
				diagnostic = {
					Status::LimitExceeded, {}, "timeline", "source step exceeds the timeline undo budget"
				};
				return false;
			}
			document = std::move(candidate);
			playback = prepared.Playback;
			documentChanged = true;
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				Status::LimitExceeded, {}, "timeline", "source step could not stage its timeline edit"
			};
			return false;
		} catch (const std::length_error &) {
			diagnostic = {
				Status::LimitExceeded, {}, "timeline", "source step exceeds the document storage limit"
			};
			return false;
		}
	}
} // namespace studio::detail
