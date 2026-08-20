#include <engine/render/Readback.hpp>

namespace engine::render {

	namespace {
		size_t BucketOf(uint8_t value) {
			return static_cast<size_t>(value) * HISTOGRAM_BUCKETS / 256;
		}

		void Count(ChannelHistogram &channel, uint8_t value) {
			channel.Buckets[BucketOf(value)]++;
			channel.Minimum = value < channel.Minimum ? value : channel.Minimum;
			channel.Maximum = value > channel.Maximum ? value : channel.Maximum;
			channel.Counted++;
		}
	}

	ImageHistogram Histogram(std::span<const uint32_t> pixels) {
		ImageHistogram out;
		for (const uint32_t pixel : pixels) {
			Count(out.Blue, static_cast<uint8_t>(pixel & 0xFFu));
			Count(out.Green, static_cast<uint8_t>((pixel >> 8) & 0xFFu));
			Count(out.Red, static_cast<uint8_t>((pixel >> 16) & 0xFFu));
			Count(out.Alpha, static_cast<uint8_t>((pixel >> 24) & 0xFFu));
		}
		return out;
	}

	ImageHistogram HistogramRgba(std::span<const uint32_t> pixels) {
		ImageHistogram out;
		for (const uint32_t pixel : pixels) {
			Count(out.Red, static_cast<uint8_t>(pixel & 0xFFu));
			Count(out.Green, static_cast<uint8_t>((pixel >> 8) & 0xFFu));
			Count(out.Blue, static_cast<uint8_t>((pixel >> 16) & 0xFFu));
			Count(out.Alpha, static_cast<uint8_t>((pixel >> 24) & 0xFFu));
		}
		return out;
	}

	void PendingReadback::Submitted(uint64_t frame) {
		InFlight = true;
		RequestedFrame = frame;
	}

	bool PendingReadback::Poll(bool signalled) {
		if (!InFlight || !signalled) {
			return false;
		}
		InFlight = false;
		HaveImage = true;
		CompletedFrame = RequestedFrame;
		return true;
	}

	uint64_t PendingReadback::Age(uint64_t frame) const {
		if (!HaveImage || frame <= CompletedFrame) {
			return 0;
		}
		return frame - CompletedFrame;
	}

	void PendingReadback::Clear() {
		InFlight = false;
		HaveImage = false;
		RequestedFrame = 0;
		CompletedFrame = 0;
	}
}
