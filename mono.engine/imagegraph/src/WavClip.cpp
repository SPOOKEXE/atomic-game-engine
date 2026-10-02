#include <engine/audio/Wav.hpp>
#include <engine/imagegraph/WavClip.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph {
	Status DecodeWavClip(
		std::span<const std::byte> bytes,
		WavClipPolicy policy,
		uint64_t byteBudget,
		AudioBit &clip,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		const auto fail = [&](Status status, const char *message) {
			diagnostic.Code = status;
			diagnostic.Message = message;
			return status;
		};
		if (policy != WavClipPolicy::Native && policy != WavClipPolicy::PixelComposer)
			return fail(Status::InvalidValue, "unknown WAV clip decode policy");
		const auto metadata = audio::InspectWav(bytes);
		if (!metadata)
			return fail(Status::InvalidValue, "WAV clip has malformed container or sample metadata");
		if (metadata->Samples > Limits::MaximumAudioClipSamples)
			return fail(Status::LimitExceeded, "WAV clip exceeds the whole-clip sample budget");
		// Both interleaved workspace and retained planar doubles coexist during conversion.
		const uint64_t overhead =
			sizeof(AudioBit) + Limits::MaximumAudioChannels * sizeof(std::vector<double>);
		if (byteBudget < overhead)
			return fail(Status::LimitExceeded, "WAV clip exceeds the conversion byte budget");
		const size_t maximumSamples = static_cast<size_t>(std::min<uint64_t>(
			Limits::MaximumAudioClipSamples, (byteBudget - overhead) / (2 * sizeof(double))
		));
		if (metadata->Samples > maximumSamples)
			return fail(Status::LimitExceeded, "WAV clip exceeds the conversion byte budget");
		AudioBit converted;
		const auto planar = [&](auto samples, auto format) {
			converted.SampleRate = format.SampleRate;
			const size_t packets = samples.size() / format.Channels;
			converted.Channels.resize(format.Channels);
			for (size_t channel = 0; channel < format.Channels; channel++) {
				auto &plane = converted.Channels[channel];
				plane.reserve(packets);
				for (size_t packet = 0; packet < packets; packet++) {
					const double sample = samples[packet * format.Channels + channel];
					if (!std::isfinite(sample)) return false;
					plane.push_back(sample);
				}
			}
			return true;
		};
		if (policy == WavClipPolicy::PixelComposer) {
			const auto decoded = audio::DecodePixelComposerWav(bytes, maximumSamples);
			if (!decoded)
				return fail(
					Status::InvalidValue,
					"WAV is malformed, unsupported by source PCM policy, or exceeds its sample budget"
				);
			if (!planar(std::span<const double>(decoded->Samples), decoded->Format))
				return fail(Status::InvalidValue, "WAV samples must be finite");
		} else {
			const auto decoded = audio::DecodeWav(bytes, maximumSamples);
			if (!decoded)
				return fail(
					Status::InvalidValue,
					"WAV is malformed, unsupported by native policy, or exceeds its sample budget"
				);
			if (!planar(decoded->Data(), decoded->Format()))
				return fail(Status::InvalidValue, "WAV samples must be finite");
		}
		clip = std::move(converted);
		return Status::Ok;
	}
}
