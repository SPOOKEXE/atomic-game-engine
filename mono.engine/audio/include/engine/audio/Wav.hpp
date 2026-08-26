#pragma once

// Turning the bytes an origin delivered into samples this engine can mix.
//
// **This decodes one container and there is exactly one other.** RIFF/WAV,
// because it is what every authoring tool writes, it is uncompressed so there
// is no codec to vendor, and `AssetKind::Audio` already classifies `.wav` at
// publish time. `Mp3.hpp` is the second, added at v0.9 because minimp3 is CC0 -
// a licence answer rather than a change of principle. Ogg and FLAC are still
// classified by the manifest and **not decoded anywhere**, and pretending to
// support them by listing an extension would be worse than the honest gap.
//
// **Every field is hostile.** A `.wav` arrives over the delivery path from an
// origin anyone can run, and the format is a
// chain of length-prefixed chunks - which is to say it is a list of numbers
// that tell a parser how far to jump. So nothing here allocates from a length
// it has not bounded against the actual buffer, no chunk header is read without
// checking there is a header's worth of bytes left, and a chunk claiming to
// extend past the end of the file is a refusal rather than a clamp.
//
// A clamp would be the tempting fix and it is the wrong one: it turns a
// truncated file into a shorter sound that plays, so the corruption is
// inaudible until somebody wonders why a footstep got quieter.
//
// @tier L12 · client

#include <engine/audio/Sample.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace engine::audio {

	// Whether some bytes look like a RIFF/WAV file at all.
	//
	// A cheap check on the first twelve bytes, for a caller deciding what a
	// blob is before spending a decode on it. **Not a substitute for decoding**
	// - it says the container is plausible and nothing about whether the rest
	// parses.
	//
	// @param bytes The file.
	// @return Whether it opens with `RIFF....WAVE`.
	bool IsWav(std::span<const std::byte> bytes);

	// Decodes a RIFF/WAV file into samples.
	//
	// Accepts the four encodings authoring tools actually write: unsigned
	// 8-bit, signed 16-bit, signed 24-bit and 32-bit float. Everything else -
	// A-law, µ-law, ADPCM, anything with a codec behind it - is refused rather
	// than half-read, because a decoder that guessed would produce noise at
	// full volume, which is the single worst failure mode this subsystem has.
	//
	// The result keeps the file's own sample rate and channel count.
	// **Resampling is the caller's**, through `SampleBuffer::ConvertTo`, so
	// that it happens once at load time with a decision visible rather than
	// implicitly here.
	//
	// @param bytes The file.
	// @return The samples, or nothing when this is not a WAV this engine
	//         decodes. Nothing means refuse the content; there is no partial
	//         result, because a partly decoded sound is one that plays.
	// @since v0.9
	std::optional<SampleBuffer> DecodeWav(std::span<const std::byte> bytes);

	// The largest file this will decode.
	//
	// A backstop rather than the real check - the real one is that every chunk
	// is bounded against the buffer it came in. This catches a `.wav` that is
	// absurd before the allocator does, and it is generous: ten minutes of
	// 48 kHz stereo float is about 230 MB.
	constexpr size_t MAXIMUM_WAV_BYTES = 512u * 1024u * 1024u;
}
