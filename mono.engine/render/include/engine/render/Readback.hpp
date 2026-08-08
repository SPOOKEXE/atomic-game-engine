#pragma once

// Getting pixels back off the GPU, and what to do with them once they arrive.
//
// **Stage 8 of `docs/PIPELINE_NODES.md`, and the half that needs no device.**
// Three of the eleven faults in §1.5 cannot be answered by looking at the
// authored graph — "is this alpha channel blank", "is this target a completely
// blank image", "how many times was this pixel shaded". Each needs the pixels,
// and pixels need a download.
//
// Two things live here, and both are arithmetic:
//
//   - `ImageHistogram`, which is the reduction that turns a downloaded image
//     into an answer. This is what makes fault 3 visible in the editor rather
//     than only in a capture.
//   - `PendingReadback`, which is the *policy* around a download: one in
//     flight, never stall, and say how old the picture is.
//
// **The device code is deliberately not here.** `Renderer` owns the transfer
// buffer and the fence; this owns the decisions about them. That is the same
// split `PassTable` and `GraphRunner` took, and for the same reason — the
// renderer is the one module a suite cannot exercise, so everything that can be
// moved out of it should be.
//
// @tier L12 · client

#include <cstdint>
#include <span>

namespace engine::render {

	// How many buckets a channel is reduced into.
	//
	// **Sixteen, not 256.** The question this answers is "is this channel
	// blank", "is it two-valued", "does it use its range" — and a sixteen-bar
	// chart says all three at a glance in a panel that is a few hundred pixels
	// wide. A 256-bin histogram is a photograph of the data; this is a
	// diagnosis.
	inline constexpr size_t HISTOGRAM_BUCKETS = 16;

	// What one channel's values look like across an image.
	//
	// @since v0.11
	struct ChannelHistogram {
		// How many pixels fell in each sixteenth of the range.
		uint32_t Buckets[HISTOGRAM_BUCKETS]{};

		// The extremes actually present.
		//
		// **Initialised inverted**, so an empty image reports `Minimum > Maximum`
		// and `Constant` is false for it rather than accidentally true.
		//@{
		uint8_t Minimum = 255;
		uint8_t Maximum = 0;
		//@}

		// How many pixels were counted.
		uint64_t Counted = 0;

		// Whether every pixel has the same value.
		//
		// **The check fault 3 wants.** The captured frame's albedo target had a
		// blank alpha channel — every texel the same — and a human found it by
		// reading a capture for half an hour. This is that, as arithmetic.
		//
		// @return False for an empty image: nothing is not the same as
		//         everything being equal, and reporting "constant" for a target
		//         nobody sampled would be the wrong warning on the wrong node.
		bool Constant() const {
			return Counted > 0 && Minimum == Maximum;
		}

		// Whether every pixel is zero, which is the specific case worth naming.
		//
		// A constant channel might be deliberate — a mask that is all ones is
		// doing its job. A channel that is all zero is either unwritten or
		// wasted, and both are worth a warning triangle.
		bool Blank() const {
			return Constant() && Maximum == 0;
		}
	};

	// The four channels of a downloaded image.
	//
	// @since v0.11
	struct ImageHistogram {
		//@{
		ChannelHistogram Red;
		ChannelHistogram Green;
		ChannelHistogram Blue;
		ChannelHistogram Alpha;
		//@}

		// Whether the whole image is one colour.
		//
		// **Fault 4, which is fault 3 over every channel at once.** The captured
		// frame had a half-resolution target the analyst called "a completely
		// blank image"; a pass whose output is uniform either did not run or did
		// not need to.
		bool Uniform() const {
			return Red.Constant() && Green.Constant() && Blue.Constant() && Alpha.Constant();
		}
	};

	// Reduces a downloaded image.
	//
	// **Takes the pixels as the transfer buffer holds them**, which is what
	// `SDL_DownloadFromGPUTexture` writes: one 32-bit word per pixel, and the
	// byte order is the texture's format. See `Channels` for the unpacking this
	// assumes.
	//
	// @param pixels One word per pixel. Empty is legal and gives an empty
	//               histogram — a target that was never downloaded is not a
	//               target that is blank.
	// @return The four channels' distributions.
	ImageHistogram Histogram(std::span<const uint32_t> pixels);

	// A download in flight, and how old the picture it produced is.
	//
	// **Never stalls, and says how stale it is.** `--capture` waits on a fence
	// because a file written one frame late is a file written wrong; a debug
	// view is the opposite — a picture one frame behind is fine and a frame that
	// hitched to fetch it is not. So this starts a download, leaves it, and
	// picks it up whenever the fence says the GPU is done.
	//
	// **The staleness is reported rather than hidden**, which is the whole
	// reason `Age` exists. A panel showing a frame-old image without saying so
	// is a panel that will one day be blamed for a bug that is a frame of
	// latency in the panel.
	//
	// **The fence is somebody else's.** This takes "is it signalled" as an
	// answer rather than asking, so the policy is testable without a GPU and the
	// device code stays in one place.
	//
	// @since v0.11
	class PendingReadback {
	  public:
		// Whether a download may be started.
		//
		// One at a time: a second request while the first is in flight would
		// need a second transfer buffer and a second fence to say anything more,
		// and what it would say is a picture the panel is not showing yet.
		bool CanRequest() const {
			return !InFlight_;
		}

		// Records that a download was submitted during this frame.
		//
		// @param frame The frame counter's value. Any monotonic count will do;
		//              this never learns what a frame is.
		void Submitted(uint64_t frame);

		// Offers the fence's state.
		//
		// @param signalled Whether the GPU has finished the copy.
		// @return True on the call that made pixels readable, so a caller knows
		//         when to map the transfer buffer. False every other time,
		//         including every call while nothing is in flight.
		bool Poll(bool signalled);

		// Whether a picture has ever arrived.
		bool HasImage() const {
			return Have_;
		}

		// Which frame the held picture was requested on.
		uint64_t ImageFrame() const {
			return Frame_;
		}

		// How many frames old the held picture is.
		//
		// @param frame The frame counter's value now.
		// @return Zero when there is no picture, or when it was requested this
		//         frame. Saturates at zero rather than wrapping if a caller
		//         hands over a frame number that went backwards.
		uint64_t Age(uint64_t frame) const;

		// Forgets everything, for a device that went away.
		void Clear();

	  private:
		bool InFlight_ = false;
		bool Have_ = false;
		uint64_t Requested_ = 0;
		uint64_t Frame_ = 0;
	};
}
