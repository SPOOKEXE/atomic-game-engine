#pragma once
#include "../AudioPayload.hpp"
#include "../NodeExecutors.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <cmath>
namespace engine::imagegraph::detail {
	struct AudioWindowGeometry {
		int64_t Width = 0;
		int64_t Step = 1;
		size_t Channels = 0;
		size_t Packets = 0;
		double Offset = 0;
		double Start = 0;
		double End = 0;
	};
	// __NodeValue_Int rounds source scalar inputs before each node reads them.
	inline int64_t AudioWindowSourceInteger(NodeContext &context, std::string_view id, int64_t fallback) {
		const Value *input = context.Find(id);
		const auto *scalar = input ? std::get_if<double>(input) : nullptr;
		if (!scalar) return context.Integer(id, fallback);
		const double lower = std::floor(*scalar);
		const double fraction = *scalar - lower;
		const double rounded =
			fraction > .5 || (fraction == .5 && std::fmod(lower, 2) != 0) ? lower + 1 : lower;
		if (!std::isfinite(rounded) || rounded < -0x1p63 || rounded >= 0x1p63) {
			context.Fail(Status::InvalidValue, "integer input must be finite and within int64 range", id);
			return fallback;
		}
		return static_cast<int64_t>(rounded);
	}

	// Executor and presentation use the same source readers and double-clamp bounds.
	inline bool
	ReadAudioWindowGeometry(NodeContext &context, const AudioBit *audio, AudioWindowGeometry &geometry) {
		if (!audio || !std::isfinite(audio->SampleRate) || audio->SampleRate <= 0 ||
			!ValidAudioPlanes(audio->Samples, audio->Channels, Limits::MaximumAudioClipSamples))
			return context.Fail(
				Status::InvalidValue,
				"Audio Window requires finite bounded planar audio and a positive sample rate",
				"audio_data"
			);
		const int64_t width = AudioWindowSourceInteger(context, "width", 4096);
		if (width < 0 || width > static_cast<int64_t>(Limits::MaximumArrayElements))
			return context.Fail(
				Status::InvalidValue,
				"Audio Window Width must be nonnegative within the sample budget",
				"width"
			);
		const int64_t step = std::max(int64_t{1}, AudioWindowSourceInteger(context, "step", 16));
		const double cursor = context.SourceChoice("cursor_location", 1);
		const int64_t unit = context.Integer("location_unit");
		if (context.FailureCode != Status::Ok) return false;
		if (unit < 0 || unit > 2)
			return context.Fail(
				Status::InvalidValue,
				"Audio Window location unit must be Bit, Second or Progress",
				"location_unit"
			);
		const size_t channels = AudioChannelCount(*audio);
		const size_t packets = AudioChannel(*audio, 0).size();
		double offset = context.Scalar("location");
		if (context.Boolean("match_timeline", true)) {
			if (!context.Timeline)
				return context.Fail(
					Status::UnsupportedExecution,
					"Match Timeline requires declared timeline FPS",
					"match_timeline"
				);
			if (!std::isfinite(context.Timeline->FramesPerSecond) || context.Timeline->FramesPerSecond <= 0)
				return context.Fail(
					Status::InvalidValue, "Match Timeline requires positive finite FPS", "match_timeline"
				);
			offset = static_cast<double>(FrameTimeToReal(GetFrameTime(context.Request))) /
					 context.Timeline->FramesPerSecond * audio->SampleRate;
		} else if (unit == 1) {
			offset *= audio->SampleRate;
		} else if (unit == 2) {
			offset = offset * (static_cast<double>(packets) / audio->SampleRate) * audio->SampleRate;
		}
		if (!std::isfinite(offset))
			return context.Fail(
				Status::InvalidValue, "Audio Window location must produce a finite sample offset", "location"
			);
		// The source rounds half-width to even, then clamps twice with packet-1 as exclusive end.
		const int64_t middle = width / 2 + ((width % 2 && (width / 2) % 2) ? 1 : 0);
		double start = 0;
		if (cursor == 0) start = offset;
		if (cursor == 1) start = offset - middle;
		if (cursor == 2) start = offset - width;
		const double last = packets ? static_cast<double>(packets - 1) : 0;
		start = std::clamp(start, 0.0, last);
		const double end = std::clamp(start + width, 0.0, last);
		start = std::clamp(end - width, 0.0, last);
		const size_t samplesPerChannel = static_cast<size_t>(std::ceil((end - start) / step));
		if (channels * samplesPerChannel > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "Audio Window output exceeds its sample or byte budget", "width"
			);
		geometry = {width, step, channels, packets, offset, start, end};
		return context.FailureCode == Status::Ok;
	}
}
