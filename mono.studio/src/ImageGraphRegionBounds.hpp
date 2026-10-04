#pragma once

#include <engine/imagegraph/SourceTimeline.hpp>

#include <algorithm>
#include <cmath>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline engine::imagegraph::TimelineSettings RegionPlaybackTimeline(const ImageGraphPlayback &playback) {
		return {
			playback.TotalFrames,
			playback.StartTick,
			playback.EndTick,
			playback.PingPong ? "pingpong"
			: playback.Loop	  ? "loop"
							  : "stop",
			playback.FramesPerSecond,
			playback.SourceBounds
		};
	}
	inline std::optional<double> SelectedRegionFirstFrame(const ImageGraphPlayback &playback) {
		const auto selected =
			playback.SelectedRegion
				? std::optional<double>(
					  double(engine::imagegraph::FrameTimeToReal(playback.SelectedRegion->first))
				  )
				: std::nullopt;
		return engine::imagegraph::SourceTimelineFirstFrame(RegionPlaybackTimeline(playback), selected);
	}
	inline std::optional<double> SelectedRegionLastFrame(const ImageGraphPlayback &playback) {
		const auto selected =
			playback.SelectedRegion
				? std::optional<double>(
					  double(engine::imagegraph::FrameTimeToReal(playback.SelectedRegion->second))
				  )
				: std::nullopt;
		return engine::imagegraph::SourceTimelineLastFrame(RegionPlaybackTimeline(playback), selected);
	}
	// Region first/reset/loop and last bounds use the native fixed-tick cadence.
	// The source controller deliberately bounces ping-pong at zero, not region start.
	// This changes the admitted clock window, never key interpolation or source VM rounding.
	inline bool AdvanceSelectedRegionPlayback(ImageGraphPlayback &playback, double elapsed) {
		using namespace engine::imagegraph;
		if (!playback.Playing) return false;
		const auto first = SelectedRegionFirstFrame(playback), last = SelectedRegionLastFrame(playback);
		if (!first || !last || *first > *last || !std::isfinite(playback.FramesPerSecond) ||
			playback.FramesPerSecond <= 0)
			return false;
		engine::imagegraph::FrameTime firstClock, lastClock;
		if (!engine::imagegraph::SplitFrameTime(*first, firstClock) ||
			!engine::imagegraph::SplitFrameTime(*last, lastClock))
			return false;
		const auto before = GetImageGraphFrame(playback);
		double frame = double(FrameTimeToReal(before));
		double remaining =
			std::isfinite(playback.Accumulator) && playback.Accumulator >= 0 ? playback.Accumulator : 0;
		remaining += std::isfinite(elapsed) ? std::clamp(elapsed, 0., .25) : 0.;
		const double duration = 1. / playback.FramesPerSecond;
		if (playback.Direction != -1 && playback.Direction != 1) playback.Direction = 1;
		for (size_t step = 0; step < 8 && remaining >= duration; ++step) {
			remaining -= duration;
			const double next = frame + (playback.PingPong ? playback.Direction : 1);
			if (next > *last) {
				if (playback.PingPong) {
					frame = std::max(0., *last - 1);
					playback.Direction = -1;
				} else if (playback.Loop)
					frame = *first;
				else {
					frame = *last;
					playback.Playing = false;
					remaining = 0;
					break;
				}
			} else if (playback.PingPong && next <= 0) {
				frame = 0;
				playback.Direction = 1;
			} else
				frame = next;
		}
		FrameTime result;
		if (!SplitFrameTime(frame, result)) return false;
		const bool changed = SetImageGraphAuthorFrame(playback, result);
		playback.Accumulator = remaining;
		return changed;
	}
}
