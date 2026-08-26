#pragma once

// Which of this module's two stacks a datagram belongs to, and who decides.
//
// `net/AGENTS.md` records that `quic/` arrived beside the datagram stack rather
// than instead of it. That leaves one question every server has to answer on
// every datagram from a stranger: *which of the two is this?* The answer has to
// be reached before a connection exists, from the bytes alone, and it has to be
// unambiguous rather than probable - a wrong guess admits a peer to the wrong
// stack, where it fails as silence.
//
// **The server chooses the mode and the client gets no vote.** `WireMode` is a
// listener's setting; a connector tries QUIC and falls back when it is refused.
// A flag on both ends that had to agree is a flag that will disagree, and the
// failure it produces is a connection that hangs with nothing saying why.
//
// This header lives in `net` rather than in `replication` because both stacks
// are `net`'s, and because `mono.network`'s advert carries the mode too -
// `replication` and `network` are the same layer, so a type either of them owned
// would be a lateral edge for the other.
//
// @tier L11 · shared

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace engine::net {

	// Which transport a session runs on.
	//
	// @since v0.19
	enum class WireKind : uint8_t {
		// `net::Packet` framing, `net::Reliability`'s window, `net::Handshake`'s
		// X25519 exchange and `net::Cipher`'s sealing. What this engine has had
		// since v0.3.
		Datagram,

		// QUIC: a stream per channel, RFC 9221 datagrams for what is unreliable,
		// and TLS 1.3 in place of the exchange.
		Quic,
	};

	// Which transports a listener will answer, and it is the server's to choose.
	//
	// **`Quic` is the default as of v0.19.** `Datagram` and `Both` are there for
	// a deployment that has a reason, and `Both` costs one extra branch per
	// datagram from a stranger rather than a second listener.
	//
	// @since v0.19
	enum class WireMode : uint8_t {
		// QUIC only. A datagram-stack handshake is refused explicitly.
		Quic,

		// The datagram stack only. A QUIC Initial is refused explicitly.
		Datagram,

		// Whichever a peer opens with.
		Both,
	};

	// Returns a stable, human-readable name for a wire.
	//
	// @param kind The wire.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(WireKind kind);

	// Returns a stable, human-readable name for a mode.
	//
	// @param mode The mode.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(WireMode mode);

	// Whether a mode answers a wire at all.
	//
	// @param mode The listener's mode.
	// @param kind The wire a peer opened with.
	// @return `true` when the listener should answer.
	// @since v0.19
	bool Serves(WireMode mode, WireKind kind);

	// Reads what `Describe(WireMode)` wrote.
	//
	// One parser, so a command line and a config file cannot disagree about
	// what "both" spells.
	//
	// @param text The name, lowercase.
	// @return The mode, or nothing when the text is not one.
	// @since v0.19
	std::optional<WireMode> ParseWireMode(std::string_view text);

	// Which stack a datagram from an unknown peer is opening.
	//
	// **The discriminator, and the two forms genuinely cannot be confused.**
	//
	// - A QUIC **long header** packet - which every Initial is, RFC 9000 §17.2 -
	//   has bit 7 of byte 0 (Header Form) set to 1 and bit 6 (Fixed Bit) set to
	//   1, so byte 0 is in `0xC0..0xFF`. Bytes 1 to 4 are the Version, and an
	//   Initial's is non-zero.
	// - A `net::Packet` opens with `Packet::MAGIC` written little-endian, so its
	//   byte 0 is always `0x41` (`'A'`) - bit 7 clear - followed by `0x54 0x4E
	//   0x31` and a two-byte version.
	//
	// Bit 7 of byte 0 alone separates them, and neither format can set it the
	// other way: the magic is a constant and QUIC's Fixed Bit pair is what makes
	// a long header a long header.
	//
	// **A QUIC *short* header is not distinguishable this way and does not need
	// to be.** A 1-RTT packet has bit 7 clear and bit 6 set, so byte 0 lands in
	// `0x40..0x7F`, which contains `0x41`. It never reaches here: a short-header
	// packet belongs to an established connection and is routed by the
	// destination connection id in it, before anything asks this question. One
	// that arrives with no connection to match is for somebody else, and the
	// full six-byte magic-and-version check below refuses it.
	//
	// @param datagram The bytes as they arrived.
	// @return The stack, or nothing when the datagram is neither.
	// @since v0.19
	std::optional<WireKind> WireOf(std::span<const std::byte> datagram);
}
