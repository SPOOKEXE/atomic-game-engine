#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/AnimationControl.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph {
	Status BuildAnimationControl(
		const AnimationControlInputs &inputs,
		const AnimationPlaybackState &prior,
		AnimationControlResult &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.animation_control.transition");
		auto finite = [](double value) {
			return std::isfinite(value) && std::abs(value) <= 9007199254740991.;
		};
		if (!finite(prior.CurrentFrame) || !finite(prior.RealFrame) || !finite(prior.LastTime) ||
			!finite(prior.RealTime) || (prior.FrameRangeStart && !finite(*prior.FrameRangeStart)) ||
			(prior.SelectionFrameStart && !finite(*prior.SelectionFrameStart)) ||
			(prior.Direction != 1 && prior.Direction != -1)) {
			diagnostic = {
				Status::InvalidValue, {}, {}, "animation control requires finite representable playback state"
			};
			return diagnostic.Code;
		}
		AnimationControlResult candidate;
		candidate.Playback = prior;
		auto &state = candidate.Playback;
		auto emit = [&](AnimationControlEffectKind kind) {
			candidate.Effects[candidate.EffectCount++] = {kind, state};
		};
		auto setFrame = [&](double frame) {
			const double base = std::floor(frame), fraction = frame - base;
			const double rounded =
				base + (fraction > .5 || (fraction == .5 && std::fmod(base, 2.) != 0.) ? 1. : 0.);
			const bool changed = state.CurrentFrame != rounded;
			state.RealFrame = frame;
			state.CurrentFrame = rounded;
			state.FrameProgress = changed;
			if (changed) {
				state.LastTime = 0;
				emit(AnimationControlEffectKind::RenderAll);
			}
		};
		const double first = state.FrameRangeStart
								 ? *state.FrameRangeStart - 1
								 : (state.SelectionFrameStart ? *state.SelectionFrameStart - 1 : 0);
		auto active = [&](bool playing) {
			state.Playing = playing;
			state.FrameProgress = true;
			state.LastTime = 0;
			state.Direction = 1;
		};
		if (!state.Rendering) {
			if (inputs.PlayPause) active(!state.Playing);
			if (inputs.Pause) active(false);
			if (inputs.Resume) active(true);
			if (inputs.PlayFromStart) {
				setFrame(first);
				state.Playing = false;
				state.LastTime = 0;
				state.Direction = 1;
				setFrame(state.Simulating ? 0 : first);
				emit(AnimationControlEffectKind::AnimationStart);
				active(true);
				state.RealTime = 0;
				emit(AnimationControlEffectKind::RenderAll);
			}
			if (inputs.PlayOnce) {
				setFrame(state.Simulating ? 0 : first);
				active(true);
				state.Rendering = true;
				state.RealTime = 0;
				emit(AnimationControlEffectKind::RenderingStart);
			}
			if (inputs.SkipFrames) {
				const long double target = static_cast<long double>(state.CurrentFrame) +
										   static_cast<long double>(inputs.SkipFramesCount);
				if (std::abs(target) > 9007199254740991.L) {
					diagnostic = {
						Status::LimitExceeded,
						{},
						"skip_frames_count",
						"animation skip exceeds native exact clock range"
					};
					return diagnostic.Code;
				}
				setFrame(static_cast<double>(target));
			}
		}
		output = candidate;
		diagnostic = {};
		return Status::Ok;
	}
	Status ResolveAnimationControl(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		const AnimationPlaybackState &prior,
		uint64_t byteBudget,
		AnimationControlResult &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.animation_control.inputs");
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &item) {
			return item.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.animation_control") {
			diagnostic = {
				Status::InvalidValue, std::string(nodeId), {}, "animation control requires its source node"
			};
			return diagnostic.Code;
		}
		const uint64_t fixed = 2 * sizeof(AnimationControlResult);
		if (byteBudget < fixed) {
			diagnostic = {
				Status::LimitExceeded, std::string(nodeId), {}, "animation control results exceed cap"
			};
			return diagnostic.Code;
		}
		EvaluationSnapshot snapshot;
		auto status =
			EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic, byteBudget - fixed);
		if (status != Status::Ok) return status;
		return ResolveAnimationControl(snapshot, nodeId, prior, byteBudget, output, diagnostic);
	}
	Status ResolveAnimationControl(
		const EvaluationSnapshot &snapshot,
		std::string_view nodeId,
		const AnimationPlaybackState &prior,
		uint64_t byteBudget,
		AnimationControlResult &output,
		Diagnostic &diagnostic
	) {
		const uint64_t fixed = 2 * sizeof(AnimationControlResult);
		if (byteBudget < fixed || snapshot.RetainedBytes() > byteBudget - fixed) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"animation control snapshot and results exceed cap"
			};
			return diagnostic.Code;
		}
		AnimationControlInputs inputs;
		const std::array<std::pair<std::string_view, bool *>, 6> bools{
			{{"play_pause", &inputs.PlayPause},
			 {"pause", &inputs.Pause},
			 {"resume", &inputs.Resume},
			 {"play_from_start", &inputs.PlayFromStart},
			 {"play_once", &inputs.PlayOnce},
			 {"skip_frames", &inputs.SkipFrames}}
		};
		for (const auto &[port, target] : bools) {
			const auto value =
				std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &item) {
					return item.Port == port;
				});
			if (value == snapshot.Values().end() || !std::holds_alternative<bool>(value->Data)) {
				diagnostic = {
					Status::InvalidValue,
					std::string(nodeId),
					std::string(port),
					"animation trigger requires resolved scalar boolean"
				};
				return diagnostic.Code;
			}
			*target = std::get<bool>(value->Data);
		}
		const auto count =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &item) {
				return item.Port == "skip_frames_count";
			});
		if (count == snapshot.Values().end() || !std::holds_alternative<int64_t>(count->Data)) {
			diagnostic = {
				Status::InvalidValue,
				std::string(nodeId),
				"skip_frames_count",
				"animation skip requires resolved scalar integer"
			};
			return diagnostic.Code;
		}
		inputs.SkipFramesCount = std::get<int64_t>(count->Data);
		const auto status = BuildAnimationControl(inputs, prior, output, diagnostic);
		if (status != Status::Ok) diagnostic.NodeId = std::string(nodeId);
		return status;
	}
} // namespace engine::imagegraph
