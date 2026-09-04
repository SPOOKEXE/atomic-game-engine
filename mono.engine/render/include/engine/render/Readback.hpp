#pragma once

// arch-waiver public-header: forward renderer API. Rendering hosts expose this
// complete asynchronous readback contract to their consumers.

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

	// How many buckets each channel's distribution is reduced to.
	//
	// Sixteen rather than 256, because what a preview answers is "is this
	// attachment blank, flat, or carrying something" - and a person reading a
	// thumbnail-sized bar chart cannot tell 256 columns apart anyway.
	inline constexpr size_t HISTOGRAM_BUCKETS = 16;

	// One channel's distribution, reduced from a downloaded image.
	struct ChannelHistogram {
		// How many samples fell in each bucket, lowest value first.
		uint32_t Buckets[HISTOGRAM_BUCKETS]{};

		// The extremes actually seen. Initialised inverted so that the first
		// sample replaces both, which is what makes an empty histogram report
		// `Minimum > Maximum` rather than a plausible black.
		//@{
		uint8_t Minimum = 255;
		uint8_t Maximum = 0;
		//@}

		// How many samples were reduced. Zero means nothing was read, which is
		// a different thing from an image that is uniformly zero.
		uint64_t Counted = 0;

		// Whether every sample had the same value.
		//
		// **The question a render target preview is really asking.** A pass that
		// wrote nothing and a pass that wrote one colour everywhere look
		// identical as a picture and are different bugs.
		//
		// @return `true` when something was counted and it never varied.
		bool Constant() const {
			return Counted > 0 && Minimum == Maximum;
		}

		// Whether the channel is uniformly zero - the shape an attachment has
		// when the pass that should have filled it did not run.
		//
		// @return `true` when constant and that constant is zero.
		bool Blank() const {
			return Constant() && Maximum == 0;
		}
	};

	// All four channels of one downloaded image.
	struct ImageHistogram {
		// One distribution per channel, in memory order.
		//@{
		ChannelHistogram Red;
		ChannelHistogram Green;
		ChannelHistogram Blue;
		ChannelHistogram Alpha;
		//@}

		// Whether every channel is constant, so the whole image is one colour.
		//
		// @return `true` when no channel varied.
		bool Uniform() const {
			return Red.Constant() && Green.Constant() && Blue.Constant() && Alpha.Constant();
		}
	};

	// Reduces BGRA8 pixels from an SDL GPU texture download.
	//
	// @param pixels One packed pixel per element.
	// @return The four channels' distributions.
	ImageHistogram Histogram(std::span<const uint32_t> pixels);

	// The same reduction for RGBA8 graph attachments.
	//
	// **A second entry point rather than a swizzle flag**, because the two
	// callers each know statically which layout they have and a runtime flag
	// would be one more thing to pass wrongly.
	//
	// @param pixels One packed pixel per element.
	// @return The four channels' distributions.
	ImageHistogram HistogramRgba(std::span<const uint32_t> pixels);

	// One in-flight texture download, and whether its pixels have arrived.
	//
	// **State and no device.** The renderer owns the transfer buffer and the
	// fence; this owns the question "may I ask for another one yet", which is
	// the half that has to be right and the half a test can reach without a GPU.
	class PendingReadback {
	  public:
		// Whether a new download may be started.
		//
		// One at a time: a second request while the first is outstanding would
		// need a second transfer buffer, and a preview that is one frame behind
		// is not worth that.
		//
		// @return `true` when nothing is in flight.
		bool CanRequest() const {
			return !InFlight;
		}

		// Records that a download was submitted on `frame`.
		//
		// @param frame The frame that asked, for `Age` to measure against.
		void Submitted(uint64_t frame);

		// Advances the request by one frame's worth of fence state.
		//
		// @param signalled Whether the renderer's fence says the copy is done.
		// @return `true` when this call is the one that completed it, so a
		//         caller reads the pixels exactly once.
		bool Poll(bool signalled);

		// Whether a completed image is held.
		//
		// @return `true` once a download has finished and not been cleared.
		bool HasImage() const {
			return HaveImage;
		}

		// Which frame the held image came from.
		//
		// @return The frame number passed to `Submitted` for that download.
		uint64_t ImageFrame() const {
			return CompletedFrame;
		}

		// How stale the held image is.
		//
		// @param frame The frame asking.
		// @return Frames elapsed since the held image's own, which is what a
		//         panel needs to say "this preview is old" rather than showing
		//         it as current.
		uint64_t Age(uint64_t frame) const;

		// Drops the held image and any request, so the next `CanRequest` is
		// true. What a resource changing shape underneath the preview calls.
		void Clear();

	  private:
		// Whether a download has been submitted and not yet completed.
		bool InFlight = false;

		// Whether a completed image is held.
		bool HaveImage = false;

		// The frames the outstanding request and the held image belong to.
		//@{
		uint64_t RequestedFrame = 0;
		uint64_t CompletedFrame = 0;
		//@}
	};
}
