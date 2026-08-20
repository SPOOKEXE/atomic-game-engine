#pragma once

// Device-free policy and image analysis for render resource previews.
//
// The renderer owns transfer buffers and fences. This owns the nonblocking
// request state and the histogram produced after pixels arrive, so both can be
// tested without a GPU.
//
// @tier L12 · client

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::render {

	inline constexpr size_t HISTOGRAM_BUCKETS = 16;

	struct ChannelHistogram {
		uint32_t Buckets[HISTOGRAM_BUCKETS]{};
		uint8_t Minimum = 255;
		uint8_t Maximum = 0;
		uint64_t Counted = 0;

		bool Constant() const {
			return Counted > 0 && Minimum == Maximum;
		}

		bool Blank() const {
			return Constant() && Maximum == 0;
		}
	};

	struct ImageHistogram {
		ChannelHistogram Red;
		ChannelHistogram Green;
		ChannelHistogram Blue;
		ChannelHistogram Alpha;

		bool Uniform() const {
			return Red.Constant() && Green.Constant() && Blue.Constant() && Alpha.Constant();
		}
	};

	// Reduces BGRA8 pixels from an SDL GPU texture download.
	ImageHistogram Histogram(std::span<const uint32_t> pixels);

	// The same reduction for RGBA8 graph attachments.
	ImageHistogram HistogramRgba(std::span<const uint32_t> pixels);

	class PendingReadback {
	  public:
		bool CanRequest() const {
			return !InFlight;
		}

		void Submitted(uint64_t frame);
		bool Poll(bool signalled);

		bool HasImage() const {
			return HaveImage;
		}

		uint64_t ImageFrame() const {
			return CompletedFrame;
		}

		uint64_t Age(uint64_t frame) const;
		void Clear();

	  private:
		bool InFlight = false;
		bool HaveImage = false;
		uint64_t RequestedFrame = 0;
		uint64_t CompletedFrame = 0;
	};
}
