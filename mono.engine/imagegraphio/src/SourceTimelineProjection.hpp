#pragma once

#include <engine/imagegraph/SourceTimeline.hpp>

#include <cmath>
#include <nlohmann/json.hpp>

namespace engine::imagegraphio::edit_detail {
	// Saved source bounds and total change independently. Playback normalization is another event.
	inline bool ProjectSourceTimeline(
		nlohmann::ordered_json &root,
		const std::optional<imagegraph::TimelineSettings> &previous,
		const std::optional<imagegraph::TimelineSettings> &desired,
		imagegraph::Diagnostic &diagnostic
	) {
		using namespace imagegraph;
		if (previous == desired) return true;
		const auto fail = [&](std::string message) {
			diagnostic = {Status::UnsupportedExecution, {}, "timeline", std::move(message)};
			return false;
		};
		if (!previous || !desired || !root.contains("animator") || !root["animator"].is_object())
			return fail("PXC timeline edit requires a mapped saved animator");
		const auto &timeline = *desired;
		if (!timeline.SourceBounds || !ValidSourceAuthoringFrameBounds(*timeline.SourceBounds) ||
			!timeline.Frames || timeline.Frames > Limits::MaximumTick ||
			!std::isfinite(timeline.FramesPerSecond) || timeline.FramesPerSecond <= 0)
			return fail(
				"PXC timeline edit requires retained source bound intent within native authoring limits"
			);
		TimelineSettings projection;
		projection.Frames = timeline.Frames;
		projection.SourceBounds = timeline.SourceBounds;
		if (ProjectSourceTimelineWindow(projection, diagnostic) != Status::Ok ||
			projection.First != timeline.First || projection.Last != timeline.Last)
			return fail("PXC bounded timeline window disagrees with exact source bounds");
		int playback = -1;
		if (timeline.Playback == "loop") playback = 0;
		if (timeline.Playback == "stop") playback = 1;
		if (timeline.Playback == "pingpong") playback = 2;
		if (playback < 0) return fail("PXC timeline playback has no source enum");
		auto &animator = root["animator"];
		const auto bound = [&](const char *name, const SourceAuthoringFrameBound &value) {
			const auto old = animator.find(name);
			if (value.Presence == SourceFrameBoundPresence::Missing) {
				if (old != animator.end()) animator.erase(old);
			} else if (value.Presence == SourceFrameBoundPresence::Null) {
				if (old == animator.end() || !old->is_null()) animator[name] = nullptr;
			} else {
				const double number = double(FrameTimeToReal(value.Value));
				if (old != animator.end() && old->is_number() && old->get<double>() == number) return;
				if (value.Value.Subframe == 0)
					animator[name] =
						value.Value.NegativeFrame ? -int64_t(value.Value.Tick) : int64_t(value.Value.Tick);
				else
					animator[name] = number;
			}
		};
		bound("frame_range_start", timeline.SourceBounds->Start);
		bound("frame_range_end", timeline.SourceBounds->End);
		if (previous->Frames != timeline.Frames) animator["frames_total"] = timeline.Frames;
		if (previous->Playback != timeline.Playback) animator["playback"] = playback;
		if (previous->FramesPerSecond != timeline.FramesPerSecond)
			animator["framerate"] = timeline.FramesPerSecond;
		return true;
	}
}
