#pragma once

// The second decoder, and the reason there is a second one.
//
// `Wav.hpp` said MP3 was "a vendored codec and a licence decision" and left the
// gap honest rather than listing an extension it could not decode. The decision
// went the other way at v0.9 because **minimp3 is CC0** - no attribution
// obligation, no patent grant to read, nothing that follows a shipped game -
// and because the format a person actually has a music file in is this one.
// Ogg and FLAC are still classified by the manifest and still not decoded here.
//
// **A compressed format can lie about how big it is, and that is the whole
// difference from WAV.** A RIFF file's chunk lengths are bounded against the
// bytes that arrived, so the worst a malformed one does is decode short. An
// MP3 frame is about a hundred bytes and expands to 1152 frames of stereo -
// nine kilobytes - so a small file can ask for gigabytes, and the input's
// length bounds nothing. The output is therefore bounded directly, which is the
// same rule delivery already applies to a Zstd frame: **size the result from
// something other than the attacker's number, and refuse rather than truncate.**
//
// **A stream that changes shape mid-file is refused.** MPEG allows the sample
// rate and channel count to differ frame to frame, and no sound anybody
// authored does that. Honouring it would mean resampling inside the decode
// loop; ignoring it would mean interleaving mono frames into a stereo buffer
// and shifting every channel after them. Refusing is the only one of the three
// that cannot be silently wrong.
//
// @tier L12 · client

#include <engine/audio/Sample.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace engine::audio {

	// Whether some bytes look like an MPEG audio file at all.
	//
	// An ID3v2 tag or a frame sync in the first bytes. **Weaker than `IsWav`
	// and deliberately so**: MP3 has no container and no magic number, only a
	// sync word that eleven set bits will match anywhere. This is for a caller
	// deciding what a blob is before spending a decode on it, and it says
	// nothing about whether the rest decodes.
	//
	// @param bytes The file.
	// @return Whether it plausibly opens an MPEG stream.
	bool IsMp3(std::span<const std::byte> bytes);

	// Decodes an MPEG-1/2 Layer I, II or III file into samples.
	//
	// The result keeps the file's own sample rate and channel count.
	// **Resampling is the caller's**, through `SampleBuffer::ConvertTo`, for
	// `DecodeWav`'s reason: it happens once at load time with the decision
	// visible rather than implicitly here.
	//
	// An ID3v2 tag at the front is skipped rather than scanned past. That is
	// not tidiness - a tag carrying cover art is a JPEG, a JPEG is arbitrary
	// bytes, and arbitrary bytes contain frame syncs. A decoder that hunted for
	// its first sync through an embedded image would occasionally start
	// decoding one.
	//
	// @param bytes The file.
	// @return The samples, or nothing when this is not an MPEG stream this
	//         engine decodes, when it changes shape mid-file, or when it
	//         decodes to more than `MAXIMUM_MP3_SAMPLES`. Nothing means refuse
	//         the content; there is no partial result, because a partly decoded
	//         sound is one that plays.
	// @since v0.9
	std::optional<SampleBuffer> DecodeMp3(std::span<const std::byte> bytes);

	// The largest file this will look at.
	//
	// A backstop, and **not the check that matters** - see below. It catches a
	// `.mp3` that is absurd before the decoder walks it.
	constexpr size_t MAXIMUM_MP3_BYTES = 128u * 1024u * 1024u;

	// The most samples a decode may produce.
	//
	// **This is the real bound**, because the input's length does not imply the
	// output's: MPEG's smallest frame is a few dozen bytes and every frame
	// yields 1152 samples per channel, so a file two orders of magnitude
	// smaller than this can ask for it all. Counted in samples rather than
	// bytes because that is what the buffer holds and what the loop can check
	// before it appends, and refused rather than truncated for `DecodeWav`'s
	// reason: a truncated sound plays.
	//
	// 512 MB of float samples, which is the ceiling `MAXIMUM_WAV_BYTES` puts on
	// a file, here on the result instead. About twenty-three minutes of 48 kHz
	// stereo - longer than any music a level streams and far shorter than a
	// machine's memory.
	constexpr size_t MAXIMUM_MP3_SAMPLES = 128u * 1024u * 1024u;
}
