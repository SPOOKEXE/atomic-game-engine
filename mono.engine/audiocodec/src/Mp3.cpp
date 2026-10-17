#include <engine/audio/Mp3.hpp>
#include <engine/core/Metrics.hpp>

// **`MINIMP3_FLOAT_OUTPUT` before the header, and it is not a preference.**
// Without it the decoder synthesises to `int16_t` and every sample would then
// be divided back into a float here - a round trip through sixteen bits that
// costs a pass over the whole track and loses the headroom `Sample.hpp` exists
// to keep. minimp3's own float path is the same code with the last conversion
// removed.
//
// **`MINIMP3_IMPLEMENTATION` is defined in this translation unit and nowhere
// else.** These are single-header libraries: a second definition is a duplicate
// symbol at link time, which is a confusing failure a long way from its cause.
//
// **The formatter is turned off for exactly these four lines**, because it
// sorts includes and would eventually put one between a define and the header
// it configures. Both defines change what `minimp3.h` compiles to; a sort that
// separated them from it would silently switch the decoder back to sixteen-bit
// output, which is a wrong sample scale rather than a build error.
// clang-format off
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

// MPEG audio, decoded defensively.
//
// The safety here is one sentence and it is a different sentence from WAV's:
// **the output is bounded, because the input's length bounds nothing.** A
// hundred-byte frame expands to nine kilobytes, so a file that arrived intact
// and small can still ask for every byte a machine has.

namespace engine::audio {
	namespace {
		// An ID3v2 tag header: "ID3", two version bytes, flags, four
		// syncsafe length bytes.
		constexpr size_t ID3V2_HEADER_BYTES = 10;

		// Set in an ID3v2 header's flag byte when a footer follows the tag,
		// which adds another header's worth to skip.
		constexpr uint8_t ID3V2_FOOTER_FLAG = 0x10;

		// How far into a file a first frame sync may sit and still count as
		// "this looks like an MP3". Enough for a small tag or some junk, and
		// not so much that any file with a `0xFF 0xEx` pair in it qualifies.
		constexpr size_t SYNC_SEARCH_BYTES = 8192;

		uint8_t At(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint8_t>(bytes[offset]);
		}

		// Whether two bytes are an MPEG frame sync: eleven set bits, and a
		// version and layer that are not the reserved encodings.
		//
		// Checking the reserved values matters more than it looks. Eleven set
		// bits occur once every 2048 random byte pairs, so a sync test alone
		// finds one in any few kilobytes of anything; requiring the two fields
		// after it to be legal is most of what separates a frame from a
		// coincidence.
		bool IsSync(uint8_t first, uint8_t second) {
			const bool sync = first == 0xFF && (second & 0xE0) == 0xE0;
			const bool version = (second & 0x18) != 0x08; // 01 is reserved
			const bool layer = (second & 0x06) != 0x00;	  // 00 is reserved
			return sync && version && layer;
		}

		// How many bytes of ID3v2 tag sit at the front, if any.
		//
		// **Skipped rather than scanned past.** A tag holding cover art holds a
		// JPEG, a JPEG is arbitrary bytes, and arbitrary bytes contain frame
		// syncs - so a decoder that hunted for its first frame through one
		// would sometimes start decoding an image. Every field here is bounded
		// against what actually arrived before it is used.
		size_t Id3v2Bytes(std::span<const std::byte> bytes) {
			if (bytes.size() < ID3V2_HEADER_BYTES) {
				return 0;
			}
			if (At(bytes, 0) != 'I' || At(bytes, 1) != 'D' || At(bytes, 2) != '3') {
				return 0;
			}

			// Syncsafe: seven bits per byte, so that a length can never contain
			// a byte that looks like a frame sync. A reader that treated it as
			// a plain big-endian integer would be up to a factor of two long on
			// any tag over 128 bytes.
			size_t declared = 0;
			for (size_t index = 6; index < ID3V2_HEADER_BYTES; ++index) {
				const uint8_t part = At(bytes, index);
				if ((part & 0x80) != 0) {
					// Not syncsafe, so this is not a length. Treat the whole
					// header as junk rather than jumping by a number that means
					// something else.
					return 0;
				}
				declared = (declared << 7) | part;
			}

			size_t total = ID3V2_HEADER_BYTES + declared;
			if ((At(bytes, 5) & ID3V2_FOOTER_FLAG) != 0) {
				total += ID3V2_HEADER_BYTES;
			}
			// A tag claiming to run past the end of what arrived skips
			// everything, which leaves nothing to decode - the honest outcome
			// for a file that is a truncated tag and no audio.
			return std::min(total, bytes.size());
		}
	}

	bool IsMp3(std::span<const std::byte> bytes) {
		const size_t start = Id3v2Bytes(bytes);
		if (start > 0 && start < bytes.size()) {
			// A tag is enough on its own: nothing else writes one.
			return true;
		}
		if (start >= bytes.size()) {
			return false;
		}

		const size_t limit = std::min(bytes.size() - 1, start + SYNC_SEARCH_BYTES);
		for (size_t at = start; at < limit; ++at) {
			if (IsSync(At(bytes, at), At(bytes, at + 1))) {
				return true;
			}
		}
		return false;
	}

	std::optional<SampleBuffer> DecodeMp3(std::span<const std::byte> bytes) {
		const auto refuse = []() -> std::optional<SampleBuffer> {
			core::Metrics::Count("audio.mp3.refused", 1.0);
			return std::nullopt;
		};

		if (bytes.size() > MAXIMUM_MP3_BYTES || !IsMp3(bytes)) {
			return refuse();
		}

		mp3dec_t decoder;
		mp3dec_init(&decoder);

		// One frame's worth, reused. The decoder writes at most this per call
		// and the figure is the library's own - sizing it from anything else
		// would be a buffer overrun the day a layer II file arrives.
		std::array<float, MINIMP3_MAX_SAMPLES_PER_FRAME> frame{};

		std::vector<float> samples;
		AudioFormat shape;
		bool haveShape = false;

		size_t at = Id3v2Bytes(bytes);
		while (at < bytes.size()) {
			const size_t remaining = bytes.size() - at;
			mp3dec_frame_info_t info{};
			const int decoded = mp3dec_decode_frame(
				&decoder,
				reinterpret_cast<const uint8_t *>(bytes.data()) + at,
				static_cast<int>(std::min<size_t>(remaining, std::numeric_limits<int>::max())),
				frame.data(),
				&info
			);

			if (info.frame_bytes <= 0) {
				// Nothing decodable left. Not an error: a trailing ID3v1 tag,
				// a Lame footer or a few bytes of junk end a great many real
				// files, and what was decoded before it is the sound.
				break;
			}
			at += static_cast<size_t>(info.frame_bytes);

			if (decoded == 0) {
				// A region the decoder skipped - junk, or the first frame of a
				// stream it is still syncing to.
				continue;
			}

			if (!haveShape) {
				shape.SampleRate = static_cast<uint32_t>(info.hz);
				shape.Channels = static_cast<uint16_t>(info.channels);
				if (!shape.IsValid()) {
					return refuse();
				}
				haveShape = true;
			} else if (static_cast<uint32_t>(info.hz) != shape.SampleRate ||
					   static_cast<uint16_t>(info.channels) != shape.Channels) {
				// A stream that changes shape mid-file. Honouring it means
				// resampling inside the decode loop; ignoring it means writing
				// mono frames into a stereo buffer and shifting every channel
				// after them. Refusing is the only one that cannot be quietly
				// wrong - `Mp3.hpp` carries the argument.
				return refuse();
			}

			const size_t produced = static_cast<size_t>(decoded) * static_cast<size_t>(info.channels);
			if (produced > frame.size() || samples.size() + produced > MAXIMUM_MP3_SAMPLES) {
				// The bound that matters, checked before the append rather than
				// after: this is the one an attacker's file controls.
				return refuse();
			}
			samples.insert(samples.end(), frame.begin(), frame.begin() + static_cast<ptrdiff_t>(produced));
		}

		if (!haveShape || samples.empty()) {
			return refuse();
		}

		core::Metrics::Count("audio.mp3.samples", static_cast<double>(samples.size()));
		return SampleBuffer(shape, samples);
	}
}
