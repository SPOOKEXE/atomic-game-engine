#include "AudioPayload.hpp"
#include "TimelineDrivers.hpp"

#include <engine/imagegraph/WavPreview.hpp>

#include <cmath>
#include <limits>

namespace engine::imagegraph {
	namespace {
		Status Fail(Diagnostic &diagnostic, Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		}
	}
	Status MakeWavPreviewIntent(
		const WavPreviewControls &controls,
		const WavPreviewTimeline &timeline,
		const WavPreviewObservation &observed,
		WavPreviewIntent &intent,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (!observed.Known)
			return Fail(diagnostic, Status::InvalidValue, "audio preview state is incomplete");
		if (!std::isfinite(timeline.Frame) || std::abs(timeline.Frame) > Limits::MaximumTick ||
			!std::isfinite(timeline.FramesPerSecond) || timeline.FramesPerSecond <= 0 ||
			!std::isfinite(controls.Gain) || !std::isfinite(controls.ShiftSeconds) ||
			controls.SampleRate == 0 || controls.Frames > Limits::MaximumAudioClipSamples ||
			timeline.FirstFrame > Limits::MaximumTick)
			return Fail(
				diagnostic, Status::InvalidValue, "audio preview controls must be finite and bounded"
			);
		WavPreviewIntent candidate;
		candidate.Stop = observed.Playing &&
						 (!controls.Play || !timeline.Playing || timeline.Frame == timeline.FirstFrame);
		candidate.Start = controls.Play && timeline.Playing && (!observed.Playing || candidate.Stop);
		if (candidate.Start) {
			if (controls.Frames == 0)
				return Fail(diagnostic, Status::UnsupportedExecution, "empty WAV has no audio preview");
			candidate.CursorFrames =
				(timeline.Frame / timeline.FramesPerSecond - controls.ShiftSeconds) * controls.SampleRate;
			candidate.Gain = controls.Gain;
			if (!std::isfinite(candidate.CursorFrames) || candidate.CursorFrames < 0 ||
				candidate.CursorFrames >= controls.Frames)
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview offset is outside the source clip"
				);
			if (controls.Gain < 0 || controls.Gain > std::numeric_limits<float>::max())
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview gain is outside the native backend range"
				);
		}
		intent = candidate;
		return Status::Ok;
	}
	Status BuildWavPreviewSamples(
		const AudioBit &clip, uint64_t byteBudget, std::vector<float> &samples, Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (!detail::ValidAudioPlanes(clip.Samples, clip.Channels, Limits::MaximumAudioClipSamples) ||
			!std::isfinite(clip.SampleRate) || clip.SampleRate <= 0 ||
			std::floor(clip.SampleRate) != clip.SampleRate ||
			clip.SampleRate > std::numeric_limits<uint32_t>::max())
			return Fail(diagnostic, Status::InvalidValue, "audio preview requires a finite planar WAV clip");
		const auto &plane = detail::AudioChannel(clip, 0);
		if (plane.empty())
			return Fail(diagnostic, Status::UnsupportedExecution, "empty WAV has no audio preview");
		if (byteBudget < sizeof(std::vector<float>) ||
			plane.size() > (byteBudget - sizeof(std::vector<float>)) / sizeof(float))
			return Fail(diagnostic, Status::LimitExceeded, "audio preview exceeds the resident byte budget");
		for (double sample : plane) {
			const double quantized = detail::DriverRoundHalfEven(sample * 16384);
			if (!std::isfinite(quantized) || quantized < -32768 || quantized > 32767)
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview sample exceeds the source PCM16 range"
				);
		}
		std::vector<float> candidate;
		candidate.reserve(plane.size());
		for (double sample : plane)
			candidate.push_back(static_cast<float>(detail::DriverRoundHalfEven(sample * 16384) / 32768));
		samples = std::move(candidate);
		return Status::Ok;
	}
	Status WavPreviewSyncFrames(
		const WavPreviewControls &controls, double framesPerSecond, uint64_t &frames, Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (controls.SampleRate == 0 || controls.Frames > Limits::MaximumAudioClipSamples ||
			!std::isfinite(framesPerSecond) || framesPerSecond <= 0)
			return Fail(
				diagnostic, Status::InvalidValue, "sync length requires a valid frame rate and WAV clip"
			);
		const double count =
			std::ceil(static_cast<double>(controls.Frames) / controls.SampleRate * framesPerSecond);
		if (!std::isfinite(count) || count > Limits::MaximumTick)
			return Fail(
				diagnostic, Status::LimitExceeded, "WAV sync length exceeds the timeline frame budget"
			);
		frames = std::max(uint64_t{1}, static_cast<uint64_t>(count) + 1);
		return Status::Ok;
	}
}
