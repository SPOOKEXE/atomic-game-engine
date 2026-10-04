#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph {
	inline bool ValidSourceAuthoringFrameBound(const SourceAuthoringFrameBound &bound) {
		switch (bound.Presence) {
		case SourceFrameBoundPresence::Missing:
		case SourceFrameBoundPresence::Null:
			return bound.Value == FrameTime{};
		case SourceFrameBoundPresence::Explicit:
			return ValidFrameTime(bound.Value, Limits::MaximumTick + 1);
		}
		return false;
	}
	inline bool ValidSourceAuthoringFrameBounds(const SourceAuthoringFrameBounds &bounds) {
		return ValidSourceAuthoringFrameBound(bounds.Start) && ValidSourceAuthoringFrameBound(bounds.End);
	}
	// Selection is a separate transient source observation; saved explicit endpoints take priority.
	inline std::optional<double> SourceTimelineFirstFrame(
		const TimelineSettings &timeline, std::optional<double> selectedRegionStart = std::nullopt
	) {
		if (!timeline.SourceBounds) return double(timeline.First);
		if (!ValidSourceAuthoringFrameBounds(*timeline.SourceBounds)) return std::nullopt;
		const auto &start = timeline.SourceBounds->Start;
		if (start.Presence == SourceFrameBoundPresence::Explicit)
			return double(FrameTimeToReal(start.Value)) - 1;
		if (selectedRegionStart) {
			if (!std::isfinite(*selectedRegionStart)) return std::nullopt;
			return *selectedRegionStart - 1;
		}
		return 0.;
	}
	inline std::optional<double> SourceTimelineLastFrame(
		const TimelineSettings &timeline, std::optional<double> selectedRegionEnd = std::nullopt
	) {
		if (!timeline.SourceBounds) return double(timeline.Last);
		if (!ValidSourceAuthoringFrameBounds(*timeline.SourceBounds)) return std::nullopt;
		const auto &end = timeline.SourceBounds->End;
		if (end.Presence == SourceFrameBoundPresence::Explicit) return double(FrameTimeToReal(end.Value)) - 1;
		if (selectedRegionEnd) {
			if (!std::isfinite(*selectedRegionEnd)) return std::nullopt;
			return *selectedRegionEnd - 1;
		}
		if (!timeline.Frames) return std::nullopt;
		return double(timeline.Frames - 1);
	}
	// First/Last are a bounded integral UI window, never a replacement for saved source endpoints.
	inline Status ProjectSourceTimelineWindow(TimelineSettings &timeline, Diagnostic &diagnostic) {
		if (!timeline.Frames || timeline.Frames > Limits::MaximumTick + 1 ||
			(timeline.SourceBounds && !ValidSourceAuthoringFrameBounds(*timeline.SourceBounds))) {
			diagnostic = {
				Status::InvalidValue, {}, "timeline", "source timeline metadata is outside native bounds"
			};
			return diagnostic.Code;
		}
		if (!timeline.SourceBounds) {
			if (timeline.First > timeline.Last || timeline.Last >= timeline.Frames) {
				diagnostic = {Status::InvalidValue, {}, "timeline", "native timeline projection is invalid"};
				return diagnostic.Code;
			}
			diagnostic = {};
			return Status::Ok;
		}
		const auto first = SourceTimelineFirstFrame(timeline), last = SourceTimelineLastFrame(timeline);
		uint64_t begin = 0, end = timeline.Frames - 1;
		if (first && last && *first >= 0 && *last >= *first && *last < double(timeline.Frames) &&
			std::floor(*first) == *first && std::floor(*last) == *last) {
			begin = uint64_t(*first);
			end = uint64_t(*last);
		}
		timeline.First = begin;
		timeline.Last = end;
		diagnostic = {};
		return Status::Ok;
	}
	// The pinned animator normalizes range slots at step entry, including a paused step.
	// Callers publish this explicit transition separately from import or Match Frames.
	inline Status NormalizeSourceTimelineBounds(TimelineSettings &timeline, Diagnostic &diagnostic) {
		if (!timeline.Frames || timeline.Frames > Limits::MaximumTick + 1 ||
			(timeline.SourceBounds && !ValidSourceAuthoringFrameBounds(*timeline.SourceBounds))) {
			diagnostic = {
				Status::InvalidValue, {}, "timeline", "source timeline normalization requires valid metadata"
			};
			return diagnostic.Code;
		}
		if (!timeline.SourceBounds) {
			diagnostic = {};
			return Status::Ok;
		}
		auto next = *timeline.SourceBounds;
		const bool hasStart = next.Start.Presence == SourceFrameBoundPresence::Explicit;
		const bool hasEnd = next.End.Presence == SourceFrameBoundPresence::Explicit;
		const double start = hasStart ? double(FrameTimeToReal(next.Start.Value)) : 0;
		const double end = hasEnd ? double(FrameTimeToReal(next.End.Value)) : double(timeline.Frames);
		if ((hasStart && start != 0) || (hasEnd && end != 0)) {
			if (start == end) {
				next.Start = {SourceFrameBoundPresence::Null, {}};
				next.End = {SourceFrameBoundPresence::Null, {}};
			} else {
				FrameTime first, last;
				if (!SplitFrameTime(
						std::max(std::min(start, end), 0.), first, false, Limits::MaximumTick + 1
					) ||
					!SplitFrameTime(
						std::min(std::max(start, end), double(timeline.Frames)),
						last,
						false,
						Limits::MaximumTick + 1
					)) {
					diagnostic = {
						Status::InvalidValue, {}, "timeline", "source step produced an unsupported bound"
					};
					return diagnostic.Code;
				}
				next.Start = {SourceFrameBoundPresence::Explicit, first};
				next.End = {SourceFrameBoundPresence::Explicit, last};
			}
		}
		timeline.SourceBounds = next;
		return ProjectSourceTimelineWindow(timeline, diagnostic);
	}
}
