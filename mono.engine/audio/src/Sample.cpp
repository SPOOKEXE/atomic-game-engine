#include <engine/audio/Sample.hpp>

#include <algorithm>
#include <cmath>

namespace engine::audio {
	namespace {
		// More channels than this and there is nothing sensible to do with
		// them: the mixer places stereo, and a format claiming sixty-four
		// channels is either a lie or content this engine cannot play. Refused
		// where it is read rather than where it is mixed.
		constexpr uint16_t MAXIMUM_CHANNELS = 8;
	}

	bool AudioFormat::IsValid() const {
		return SampleRate > 0 && Channels > 0 && Channels <= MAXIMUM_CHANNELS;
	}

	SampleBuffer::SampleBuffer(AudioFormat format, size_t frames) : Shape(format) {
		if (!Shape.IsValid()) {
			Shape = AudioFormat{};
		}
		Samples.assign(frames * Shape.Channels, 0.0f);
	}

	SampleBuffer::SampleBuffer(AudioFormat format, std::span<const float> samples) : Shape(format) {
		if (!Shape.IsValid() || samples.size() % Shape.Channels != 0) {
			// A partial frame is a channel count that does not match the data,
			// and carrying it would put a stride bug in the mixer. Empty
			// rather than truncated: truncating would silently change the
			// content's length.
			Shape = AudioFormat{};
			return;
		}
		Samples.assign(samples.begin(), samples.end());
	}

	double SampleBuffer::Seconds() const {
		if (Shape.SampleRate == 0) {
			return 0.0;
		}
		return static_cast<double>(Frames()) / static_cast<double>(Shape.SampleRate);
	}

	std::span<const float> SampleBuffer::Frame(size_t frame) const {
		if (frame >= Frames()) {
			return {};
		}
		return {Samples.data() + frame * Shape.Channels, Shape.Channels};
	}

	void SampleBuffer::Silence() {
		std::fill(Samples.begin(), Samples.end(), 0.0f);
	}

	void SampleBuffer::Resize(size_t frames) {
		Samples.resize(frames * Shape.Channels, 0.0f);
	}

	size_t SampleBuffer::MixFrom(const SampleBuffer &other, float gain) {
		if (Shape != other.Shape) {
			// A no-op rather than a resample. Resampling is a decision about
			// quality and belongs where one can be made, not in the middle of
			// a sum on a thread with a deadline.
			return 0;
		}

		const size_t frames = std::min(Frames(), other.Frames());
		const size_t count = frames * Shape.Channels;
		for (size_t index = 0; index < count; ++index) {
			Samples[index] += other.Samples[index] * gain;
		}
		return frames;
	}

	float SampleBuffer::Peak() const {
		float peak = 0.0f;
		for (const float sample : Samples) {
			peak = std::max(peak, std::abs(sample));
		}
		return peak;
	}

	SampleBuffer SampleBuffer::ConvertTo(const AudioFormat &target) const {
		if (!target.IsValid() || !Shape.IsValid()) {
			return {};
		}
		if (target == Shape) {
			return *this;
		}

		const size_t sourceFrames = Frames();
		if (sourceFrames == 0) {
			return SampleBuffer(target, size_t{0});
		}

		// Rounded rather than truncated, so a rate ratio that lands just under
		// a whole frame does not drop the last one — which on a short sound is
		// an audible click at the end of every playback.
		const double ratio = static_cast<double>(target.SampleRate) / static_cast<double>(Shape.SampleRate);
		const auto targetFrames =
			static_cast<size_t>(std::llround(static_cast<double>(sourceFrames) * ratio));

		SampleBuffer out(target, targetFrames);
		std::span<float> written = out.Data();

		for (size_t frame = 0; frame < targetFrames; ++frame) {
			// Where this output frame sits in the input, as a fraction.
			const double position = static_cast<double>(frame) / ratio;
			const auto left = static_cast<size_t>(position);
			const size_t right = std::min(left + 1, sourceFrames - 1);
			const auto blend = static_cast<float>(position - static_cast<double>(left));

			for (uint16_t channel = 0; channel < target.Channels; ++channel) {
				// Channel mapping, and both directions are the naive answer
				// stated plainly rather than a matrix nobody configured:
				// mono fans out to every channel, and a source with more
				// channels than the target takes the first ones. A real
				// down-mix wants coefficients per layout, and that is a
				// decision for when there is a layout to have an opinion
				// about.
				const uint16_t sourceChannel =
					Shape.Channels == 1 ? 0 : std::min<uint16_t>(channel, Shape.Channels - 1);

				const float a = Samples[left * Shape.Channels + sourceChannel];
				const float b = Samples[right * Shape.Channels + sourceChannel];
				written[frame * target.Channels + channel] = a + (b - a) * blend;
			}
		}
		return out;
	}
}
