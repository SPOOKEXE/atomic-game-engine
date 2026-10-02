#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct AnimationControlInputs {
		bool PlayPause = false, Pause = false, Resume = false, PlayFromStart = false, PlayOnce = false,
			 SkipFrames = false;
		int64_t SkipFramesCount = 1;
	};
	// Caller playback owner supplies the source clock fields. No clock or callback
	// is owned here.
	struct AnimationPlaybackState {
		double CurrentFrame = 0, RealFrame = 0, LastTime = 0, RealTime = 0;
		bool Playing = false, Rendering = false, Simulating = false, FrameProgress = false;
		int Direction = 1;
		std::optional<double> FrameRangeStart;
		std::optional<double> SelectionFrameStart;
		bool operator==(const AnimationPlaybackState &) const = default;
	};
	enum class AnimationControlEffectKind : uint8_t { RenderAll, AnimationStart, RenderingStart };
	struct AnimationControlEffect {
		AnimationControlEffectKind Kind = AnimationControlEffectKind::RenderAll;
		AnimationPlaybackState Playback;
		bool operator==(const AnimationControlEffect &) const = default;
	};
	struct AnimationControlResult {
		AnimationPlaybackState Playback;
		// Effects retain intermediate state so callbacks observe source command
		// order.
		std::array<AnimationControlEffect, 8> Effects{};
		size_t EffectCount = 0;
		bool operator==(const AnimationControlResult &) const = default;
	};
	Status BuildAnimationControl(
		const AnimationControlInputs &, const AnimationPlaybackState &, AnimationControlResult &, Diagnostic &
	);
	Status ResolveAnimationControl(
		const Document &,
		const Plan &,
		std::string_view nodeId,
		const EvaluationRequest &,
		const AnimationPlaybackState &,
		uint64_t byteBudget,
		AnimationControlResult &,
		Diagnostic &
	);
	// Reuses an already captured input cone. No producer is executed by this overload.
	Status ResolveAnimationControl(
		const EvaluationSnapshot &,
		std::string_view nodeId,
		const AnimationPlaybackState &,
		uint64_t byteBudget,
		AnimationControlResult &,
		Diagnostic &
	);
} // namespace engine::imagegraph
