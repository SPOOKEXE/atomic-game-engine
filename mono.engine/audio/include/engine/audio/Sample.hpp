#pragma once

// What a sound *is*, once it is in this engine: interleaved 32-bit float PCM.
//
// **One internal representation, chosen rather than negotiated.** Every decoder
// converts to this on the way in and the mixer works in nothing else. The
// alternative — carrying each source's native format through the graph and
// converting at the last moment — means every processor is written N times or
// carries a switch, and the switch is in the inner loop of the one subsystem
// with a hard deadline.
//
// **Float rather than fixed point**, because a mixer sums. Sixteen-bit integers
// clip the moment two loud sounds add, so a mixer using them has to attenuate
// defensively at every stage and loses headroom it cannot get back. Floats
// carry values past ±1.0 harmlessly through the whole graph and are clamped
// exactly once, at the device. Nominal range is ±1.0 and **exceeding it inside
// the graph is legal and expected**.
//
// **Interleaved rather than planar.** Planar is faster for per-channel DSP and
// interleaved is what every device wants; this engine's graph does far more
// summing and copying than per-channel filtering, and the device conversion is
// the one that would happen on the callback thread. Revisit when a filter
// bank exists and the number says otherwise — `AGENTS.md` asks for a
// measurement beside an algorithm choice, and this one has none yet.
//
// @tier L12 · client

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::audio {

	// The sample rate the graph runs at unless a device asks for another.
	//
	// 48 kHz rather than 44.1: it is what every modern device runs natively, so
	// it is the rate that costs no resampling at the output. Content at another
	// rate is resampled once, when it is decoded.
	constexpr uint32_t DEFAULT_SAMPLE_RATE = 48000;

	// How many channels the graph mixes in.
	//
	// Stereo, and the whole engine assumes it in the one place that matters —
	// `Pan` is a stereo operation. Surround is a change to the mixer's output
	// stage rather than to the format, and it is not pretended to be supported
	// by making this a variable nothing honours.
	constexpr uint16_t CHANNELS = 2;

	// How many frames a mixer renders at once unless told otherwise.
	//
	// A *frame* is one sample per channel, so a 512-frame block is 1024 floats
	// in stereo. 512 at 48 kHz is 10.7 ms, which is the usual compromise:
	// small enough that a queued sound is not audibly late, large enough that
	// the per-block overhead is not the cost. Chosen rather than measured.
	constexpr size_t DEFAULT_BLOCK_FRAMES = 512;

	// The shape of some audio.
	//
	// @since v0.9
	struct AudioFormat {
		// Frames per second.
		uint32_t SampleRate = DEFAULT_SAMPLE_RATE;

		// Samples per frame. One for mono, two for stereo.
		uint16_t Channels = CHANNELS;

		// Whether this describes audio at all.
		//
		// @return False for a zero rate or zero channels, and for more
		//         channels than the mixer can place — a format nothing can
		//         play is refused where it is read rather than where it is
		//         mixed.
		bool IsValid() const;

		// Whether two formats are the same.
		bool operator==(const AudioFormat &other) const = default;
	};

	// Interleaved float samples, and the format they are in.
	//
	// @since v0.9
	class SampleBuffer {
	  public:
		SampleBuffer() = default;

		// Builds a silent buffer.
		//
		// @param format The shape.
		// @param frames How many frames of silence.
		SampleBuffer(AudioFormat format, size_t frames);

		// Builds a buffer over existing interleaved samples.
		//
		// @param format The shape.
		// @param samples Interleaved samples. Its length must be a whole
		//        number of frames, or the buffer comes back empty — a partial
		//        frame is a channel count that does not match the data, and
		//        carrying it would put a stride bug in the mixer.
		SampleBuffer(AudioFormat format, std::span<const float> samples);

		// The shape.
		const AudioFormat &Format() const {
			return Shape;
		}

		// How many frames — one sample per channel each.
		size_t Frames() const {
			return Shape.Channels == 0 ? 0 : Samples.size() / Shape.Channels;
		}

		// How long this is, in seconds.
		double Seconds() const;

		// Whether there is nothing here.
		bool Empty() const {
			return Samples.empty();
		}

		// The interleaved samples.
		std::span<float> Data() {
			return Samples;
		}

		// The interleaved samples.
		std::span<const float> Data() const {
			return Samples;
		}

		// One frame's samples.
		//
		// @param frame Which frame. Out of range gives an empty span rather
		//        than undefined behaviour: a mixer walking past the end of a
		//        voice is an ordinary bug and should not be a crash.
		// @return The channels of that frame.
		std::span<const float> Frame(size_t frame) const;

		// Sets every sample to zero, keeping the length and the capacity.
		void Silence();

		// Resizes to `frames`, keeping the capacity.
		//
		// New frames are silent. A buffer held across blocks stops allocating
		// after the first one that reached its high-water mark, which is the
		// point on a thread with a deadline.
		//
		// @param frames How many frames to hold.
		void Resize(size_t frames);

		// Adds another buffer's samples into this one, frame for frame.
		//
		// **The mixer's one operation**, and it is here rather than in the
		// mixer so that the bounds rule lives with the data. Mixing stops at
		// whichever buffer is shorter rather than reading past either — a bus
		// summing a voice that ended mid-block is the ordinary case, not an
		// error.
		//
		// @param other What to add. A format mismatch is a no-op rather than a
		//        resample: resampling belongs where a decision about quality
		//        can be made, not in the middle of a sum.
		// @param gain A scalar applied to `other` as it is added.
		// @return How many frames were mixed.
		size_t MixFrom(const SampleBuffer &other, float gain = 1.0f);

		// The largest absolute sample, for a meter or a test.
		//
		// @return The peak, or zero for an empty buffer.
		float Peak() const;

		// Converts to another format, resampling and re-channelling as needed.
		//
		// **Linear interpolation, and saying so matters.** It is adequate for
		// content that is close to the target rate and audibly poor for large
		// ratios; a polyphase resampler is what this wants and
		// `DATATYPES_LIBRARIES.md` lists one as a dependency this engine does
		// not have yet. It runs at load time and never on the device thread,
		// so the cost of replacing it later is a decode path rather than a
		// mixer.
		//
		// @param target The format to produce.
		// @return The converted buffer, or an empty one when `target` is
		//         invalid.
		SampleBuffer ConvertTo(const AudioFormat &target) const;

	  private:
		AudioFormat Shape;
		std::vector<float> Samples;
	};
}
