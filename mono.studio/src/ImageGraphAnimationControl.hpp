#pragma once

#include <engine/imagegraph/AnimationControl.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>

#include <algorithm>
#include <cmath>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline engine::imagegraph::AnimationPlaybackState AnimationPlayback(const ImageGraphPlayback &owner) {
		engine::imagegraph::AnimationPlaybackState state;
		state.CurrentFrame = double(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(owner)));
		state.RealFrame = owner.RealFrame;
		state.LastTime = owner.LastTime;
		state.RealTime = owner.RealTime;
		state.Playing = owner.Playing;
		state.Rendering = owner.Rendering;
		state.Simulating = owner.Simulating;
		state.FrameProgress = owner.FrameProgress;
		state.Direction = owner.Direction;
		if (owner.SourceBounds) {
			const auto &sourceStart = owner.SourceBounds->Start;
			if (sourceStart.Presence == engine::imagegraph::SourceFrameBoundPresence::Explicit &&
				engine::imagegraph::ValidSourceAuthoringFrameBound(sourceStart))
				state.FrameRangeStart = double(engine::imagegraph::FrameTimeToReal(sourceStart.Value));
		} else {
			state.FrameRangeStart = double(owner.StartTick) + 1;
		}
		return state;
	}
	// Validate the complete effect sequence before changing the owner or
	// dispatching callbacks.
	template <class Effect>
	bool ApplyAnimationControl(
		ImageGraphPlayback &owner,
		const engine::imagegraph::AnimationControlResult &result,
		Effect &&effect,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		const auto valid = [](const AnimationPlaybackState &state) {
			FrameTime frame;
			return SplitFrameTime(state.CurrentFrame, frame) && std::isfinite(state.RealFrame) &&
				   std::isfinite(state.LastTime) && std::isfinite(state.RealTime) &&
				   (state.Direction == 1 || state.Direction == -1);
		};
		if (result.EffectCount > result.Effects.size() || !valid(result.Playback)) {
			diagnostic = {Status::LimitExceeded, {}, {}, "animation control exceeds the editor clock bounds"};
			return false;
		}
		for (size_t i = 0; i < result.EffectCount; ++i)
			if (!valid(result.Effects[i].Playback)) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "animation effect exceeds the editor clock bounds"
				};
				return false;
			}
		const auto assign = [&](const AnimationPlaybackState &state) {
			FrameTime frame;
			(void)SplitFrameTime(state.CurrentFrame, frame);
			const bool changed = GetImageGraphFrame(owner) != frame || owner.Playing != state.Playing ||
								 owner.LastTime != state.LastTime;
			(void)SetImageGraphAuthorFrame(owner, frame);
			owner.Playing = state.Playing;
			owner.Rendering = state.Rendering;
			owner.Simulating = state.Simulating;
			owner.FrameProgress = state.FrameProgress;
			owner.Direction = int8_t(state.Direction);
			owner.RealFrame = state.RealFrame;
			owner.LastTime = state.LastTime;
			owner.RealTime = state.RealTime;
			if (changed) owner.Accumulator = 0;
		};
		for (size_t i = 0; i < result.EffectCount; ++i) {
			assign(result.Effects[i].Playback);
			effect(result.Effects[i].Kind);
		}
		assign(result.Playback);
		diagnostic = {};
		return true;
	}
	// A render cycle visits the authored interval once, preserving the author's
	// loop preference.
	inline bool AdvanceAnimationPlayback(ImageGraphPlayback &owner, double elapsed) {
		ImageGraphPlayback candidate = owner;
		if (candidate.Rendering) candidate.Loop = candidate.PingPong = false;
		if (candidate.Simulating && candidate.CurrentTick < candidate.StartTick) candidate.StartTick = 0;
		const bool changed = AdvanceImageGraphPlayback(candidate, elapsed);
		candidate.Loop = owner.Loop;
		candidate.PingPong = owner.PingPong;
		candidate.StartTick = owner.StartTick;
		if (!candidate.Playing) {
			candidate.Rendering = false;
			if (owner.Rendering) candidate.LastTime = 0;
		}
		if (owner.Playing && candidate.Playing) {
			candidate.LastTime = owner.RealTime;
			candidate.RealTime =
				owner.RealTime + (std::isfinite(elapsed) ? std::clamp(elapsed, 0., .25) : 0.);
		}
		candidate.RealFrame = double(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(candidate))) +
							  candidate.Accumulator * candidate.FramesPerSecond;
		candidate.FrameProgress = changed;
		const bool transition =
			changed || owner.Rendering != candidate.Rendering || owner.Playing != candidate.Playing;
		owner = candidate;
		return transition;
	}
	inline void
	RestartAnimationReplay(engine::imagegraph::CapturedFeedbackHost &replay, ImageGraphPreviewCache &cache) {
		replay.RestartCycle();
		cache.Clear();
	}

} // namespace studio::detail
