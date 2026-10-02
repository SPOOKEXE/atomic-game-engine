#pragma once

// Shared bounded planar payload checks. Samples is the legacy mono shape; Channels carries explicit planes.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace engine::imagegraph::detail {
	inline size_t
	AudioSampleCount(const std::vector<double> &samples, const std::vector<std::vector<double>> &channels) {
		size_t count = samples.size();
		for (const auto &channel : channels)
			count += channel.size();
		return count;
	}

	inline bool ValidAudioPlanes(
		const std::vector<double> &samples,
		const std::vector<std::vector<double>> &channels,
		size_t maximumSamples = Limits::MaximumAudioSamplesPerFrame
	) {
		if (!channels.empty() && !samples.empty()) return false;
		if (channels.size() > Limits::MaximumAudioChannels) return false;
		size_t count = samples.size();
		if (count > maximumSamples) return false;
		const auto finite = [](double sample) { return std::isfinite(sample); };
		if (!std::all_of(samples.begin(), samples.end(), finite)) return false;
		for (const auto &channel : channels) {
			if (channel.size() != channels.front().size() || channel.size() > maximumSamples - count)
				return false;
			if (!std::all_of(channel.begin(), channel.end(), finite)) return false;
			count += channel.size();
		}
		return true;
	}

	inline size_t AudioChannelCount(const AudioBit &audio) {
		return audio.Channels.empty() ? 1 : audio.Channels.size();
	}
	inline std::span<const double> AudioChannel(const AudioBit &audio, size_t channel) {
		return audio.Channels.empty() ? std::span<const double>(audio.Samples)
									  : std::span<const double>(audio.Channels[channel]);
	}
}
