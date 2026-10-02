#include "../AudioPayload.hpp"
#include "Families.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		bool WavFile(NodeContext &context) {
			if (context.Request.AudioClips.size() > Limits::MaximumNodes)
				return context.Fail(
					Status::LimitExceeded, "WAV assets exceed the source count budget", "path"
				);
			const Value *pathValue = context.Find("path");
			const auto *pathString = pathValue ? std::get_if<std::string>(pathValue) : nullptr;
			const std::string_view path = pathString ? std::string_view(*pathString) : std::string_view{};
			const AudioClipSource *source = nullptr;
			for (const auto &candidate : context.Request.AudioClips) {
				if (candidate.SourceId != path) continue;
				if (source)
					return context.Fail(
						Status::DuplicateId, "WAV source path selects duplicate clip assets", "path"
					);
				source = &candidate;
			}
			if (path.empty() || !source)
				return context.Fail(
					Status::InvalidValue, "WAV source path requires an explicit whole clip asset", "path"
				);
			const auto &input = source->Data;
			if (!std::isfinite(input.SampleRate) || input.SampleRate <= 0 ||
				input.SampleRate > std::numeric_limits<uint32_t>::max() ||
				std::floor(input.SampleRate) != input.SampleRate ||
				!ValidAudioPlanes(input.Samples, input.Channels, Limits::MaximumAudioClipSamples))
				return context.Fail(
					Status::InvalidValue, "WAV source requires finite bounded planar samples", "path"
				);
			const size_t channels = AudioChannelCount(input);
			const size_t packets = AudioChannel(input, 0).size();
			const bool mono = context.Boolean("mono");
			const uint64_t audioBytes =
				mono ? sizeof(std::vector<double>) + packets * sizeof(double) : PayloadOwnedBytes(input);
			// Admit the retained audio, copied path and each output name before constructing them.
			const uint64_t minimumStringCapacity = std::string{}.capacity();
			uint64_t bytes = audioBytes + std::max<uint64_t>(path.size(), minimumStringCapacity);
			for (const std::string_view port : {"data", "path", "sample_rate", "channels", "duration"})
				bytes += std::max<uint64_t>(port.size(), minimumStringCapacity);
			if (!context.ReserveOutput(bytes, "data")) return false;
			AudioBit output;
			if (mono) {
				output.SampleRate = input.SampleRate;
				output.Channels.resize(1);
				auto &plane = output.Channels.front();
				plane.resize(packets);
				for (size_t index = 0; index < packets; index++) {
					double sample = 0;
					for (size_t channel = 0; channel < channels; channel++)
						sample += AudioChannel(input, channel)[index];
					sample /= channels;
					if (!std::isfinite(sample))
						return context.Fail(
							Status::InvalidValue, "WAV mono conversion exceeded finite range", "data"
						);
					plane[index] = sample;
				}
			} else {
				output = input;
			}
			context.SetValue("data", std::move(output));
			context.SetValue("path", *pathString);
			context.SetValue("sample_rate", static_cast<int64_t>(input.SampleRate));
			context.SetValue("channels", static_cast<int64_t>(channels));
			context.SetValue("duration", static_cast<double>(packets) / input.SampleRate);
			return context.FailureCode == Status::Ok;
		}
	}

	std::span<const ExecutorEntry> AudioFileExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.wav_file_read", WavFile}};
		return ENTRIES;
	}
}
