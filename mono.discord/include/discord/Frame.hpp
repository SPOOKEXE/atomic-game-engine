#pragma once

// Discord's IPC framing: `[uint32 opcode][uint32 length][length bytes of JSON]`,
// little-endian.
//
// Pure functions over bytes. Nothing here opens anything, which is what lets
// the whole codec be tested against a `std::vector` and lets the awkward case -
// a frame split across two reads - be written down rather than waited for.
//
// **The split case is the one that matters.** A unix socket carrying a
// two-hundred-byte payload delivers it whole almost every time, so a decoder
// that assumes it will is a decoder that works on every machine it was written
// on and fails on somebody else's under load.
//
// @tier shared
// @since v0.17

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace discord {

	// What a frame is for.
	//
	// The numbers reach a wire, so this list is append-only.
	//
	// @since v0.17
	enum class Opcode : uint32_t {
		// The first frame on a connection, carrying the application id.
		Handshake = 0,

		// A command or an event. Everything after the handshake.
		Frame = 1,

		// Either end saying it is done.
		Close = 2,

		// Discord asking whether we are alive.
		Ping = 3,

		// The answer to a `Ping`.
		Pong = 4,
	};

	// The header, and therefore the smallest a frame can be.
	inline constexpr size_t FRAME_HEADER_BYTES = 8;

	// The largest frame either end will send or accept.
	//
	// Discord's own client uses the buffer size libuv gives a named pipe. Ours
	// are a few hundred bytes; the cap is here so that a length field arriving
	// from outside cannot ask for an allocation.
	inline constexpr size_t MAXIMUM_FRAME_BYTES = 64u * 1024u;

	// One decoded frame.
	//
	// @since v0.17
	struct DecodedFrame {
		// Which kind of frame this is, from the four-byte header.
		Opcode Op = Opcode::Frame;

		// The body, exactly as it arrived. JSON for every opcode that carries
		// one, and parsed by the caller rather than here - this layer's job is
		// the framing.
		std::string Payload;
	};

	// Why a buffer did not yield a frame.
	//
	// @since v0.17
	enum class DecodeResult : uint8_t {
		// A frame came out and the bytes it used were consumed.
		Ok = 0,

		// Not enough bytes yet. Nothing was consumed; read more and ask again.
		Incomplete = 1,

		// The length field is past `MAXIMUM_FRAME_BYTES`, or the opcode is not
		// one of the five. The connection is not recoverable.
		Corrupt = 2,
	};

	// Builds one frame.
	//
	// @param op      What the frame is for.
	// @param payload The JSON body. Must be under `MAXIMUM_FRAME_BYTES`.
	// @return The bytes to write, empty when `payload` is too large.
	// @since v0.17
	std::vector<std::byte> EncodeFrame(Opcode op, std::string_view payload);

	// Takes one frame off the front of `buffer`, if a whole one is there.
	//
	// **Consumes on success only.** An incomplete frame leaves `buffer`
	// untouched, so the caller's loop is "read, then decode until `Incomplete`"
	// and there is no partial state to carry between calls.
	//
	// @param buffer What has been read so far. Shortened by whatever was used.
	// @param out    Filled on `Ok`.
	// @return What happened.
	// @since v0.17
	DecodeResult DecodeFrame(std::vector<std::byte> &buffer, DecodedFrame &out);
}
