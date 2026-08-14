#pragma once

// What actually goes on the wire, and what a reader is allowed to believe.
//
// One header, on every packet, in one place. The alternative - each subsystem
// framing its own - is how two builds end up disagreeing about a length field,
// and the disagreement surfaces as a desync a long way from its cause.
//
// **Every field of an inbound packet is hostile.** `repo_layout.md` §1 says
// anyone can run a server, so a client's packets and a server's packets are both
// attacker-controlled from the other side's point of view. There is no
// "trusted direction" here and the reader has no fast path that skips a check.
//
// The header is deliberately small. It is paid on every packet - sixty times a
// second per player - so a byte here is bandwidth for the life of the product,
// which is why the channel is one byte rather than a spelled-out name and why
// there is no room for anything a receiver can derive.
//
// **The header is also the associated data of the frame under it**, so every
// field here is authenticated even though none of it is secret. A rewritten
// channel, sequence or acknowledgement fails the tag rather than being acted
// on. That is why the header is written before the payload is sealed and why
// `Read` hands back the bytes it parsed rather than only the fields.
//
// @tier L11 · shared

#include <engine/core/Bytes.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Enums.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::net {

	// The fixed header every packet carries.
	//
	// @since v0.3
	struct PacketHeader {
		// Which channel the payload belongs to, and therefore what the
		// transport promised about it.
		ChannelKind Channel = ChannelKind::Unreliable;

		// The sender's packet counter for this channel, wrapping.
		//
		// **Unreliable delivery is what this is for.** A receiver keeps the
		// highest sequence it has seen and discards anything older, because a
		// position update that arrives after a newer one is worse than useless -
		// applying it moves the world backwards. Reliable delivery uses it to
		// order and to detect a gap.
		uint16_t Sequence = 0;

		// The highest sequence the sender has received from this peer.
		//
		// Rides every packet rather than travelling as its own message. An
		// acknowledgement that needs a packet of its own doubles the packet rate
		// of a conversation that is mostly one-way, and a game is mostly
		// one-way.
		uint16_t Acknowledge = 0;

		// A bitfield of the 32 sequences before Acknowledge, one bit each.
		//
		// So a single arriving packet acknowledges up to 33, and losing one
		// acknowledgement does not force a resend of something that did in fact
		// arrive. This is the standard trick and it is here rather than in a
		// reliability layer above because it costs four bytes and saves a
		// round trip.
		uint32_t AcknowledgeBits = 0;

		// The nonce counter the payload was sealed under.
		//
		// **Carried whole, and not derived from `Sequence`.** The sequence is
		// the obvious candidate and it is the wrong one: it is 16 bits and
		// `IsNewer` exists precisely because it wraps every eighteen minutes at
		// sixty packets a second. A nonce built from a wrapping counter is a
		// nonce that repeats, and the *second* use of a ChaCha20-Poly1305 nonce
		// under one key leaks the XOR of two plaintexts and the material to
		// forge tags. Eight bytes a packet is what it costs to make that
		// impossible rather than unlikely, and the counter that fills them
		// comes from `Cipher::Sealer`, which only ever moves forward and
		// refuses at its end rather than wrapping.
		//
		// It is not a secret, and it does not need to be authenticated on its
		// own: rewriting it changes the nonce the receiver derives, so the tag
		// fails. It is covered anyway, because the whole header is.
		//
		// Zero on the handshake channel, which is the one channel with no keys
		// yet - see `Enums.hpp`.
		uint64_t Counter = 0;
	};

	// Reading and writing the wire format.
	//
	// Static, because framing has no state. Anything that needs state - the
	// sequence counters, the acknowledgement window - belongs to the connection
	// that owns them, and putting it here would make one shared framer the
	// bottleneck every connection queues behind.
	class Packet {
	  public:
		// The protocol magic, so a stray datagram on a reused port fails at its
		// first four bytes rather than somewhere confusing.
		static constexpr uint32_t MAGIC = 0x314E5441; // "ATN1"

		// The protocol version.
		//
		// **Refused when unknown, never negotiated downward.** A server that
		// speaks an old version to an old client is a server running two
		// protocols, and the second one is the one nobody tests. A version
		// mismatch is a clear disconnect with a reason a player can act on.
		static constexpr uint16_t VERSION = 1;

		// Bytes before the payload: magic, version, channel, sequence,
		// acknowledge, acknowledge bits, nonce counter, payload length.
		static constexpr size_t HEADER_BYTES = 4 + 2 + 1 + 2 + 2 + 4 + 8 + 2;

		// The largest payload one packet may carry, sealed.
		//
		// 1200 bytes, chosen rather than derived: it is the conventional safe
		// figure that survives the smallest path MTU in real use - 1280 for IPv6
		// - with room for the IP and UDP headers underneath. Sending more means
		// fragmentation, and a fragmented datagram is lost entirely when any one
		// fragment is, which multiplies the loss rate this whole design assumes
		// is small.
		//
		// **This is the sealed size, and it is not what a caller may hand over.**
		// The Poly1305 tag is part of the payload on the wire; the number a
		// caller budgets against is MAXIMUM_MESSAGE_BYTES below.
		static constexpr size_t MAXIMUM_PAYLOAD_BYTES = 1200 - HEADER_BYTES;

		// The largest message a caller may hand to a connection.
		//
		// **This is the number every budget above this module must be sized
		// against, and getting it wrong has one symptom.** A message that can
		// never fit is refused by `Link::Reserve`, and a refusal is also what
		// ordinary backpressure looks like - so a budget left at the sealed size
		// produces a message that is never sent and never reported as anything
		// but a busy link. That has cost this module a session's worth of bugs
		// already; see `replication/AGENTS.md`.
		static constexpr size_t MAXIMUM_MESSAGE_BYTES = MAXIMUM_PAYLOAD_BYTES - Cipher::OVERHEAD_BYTES;

		// Writes a packet.
		//
		// @param writer Where the bytes go.
		// @param header The header to write.
		// @param payload The payload. May be empty - a packet carrying only an
		//        acknowledgement is how a quiet connection stays alive.
		// @return False when the payload is over MAXIMUM_PAYLOAD_BYTES, in which
		//         case nothing is written.
		static bool
		Write(core::ByteWriter &writer, const PacketHeader &header, std::span<const std::byte> payload);

		// Writes the header alone, for a sender that has to authenticate it
		// before the payload exists.
		//
		// The header is the associated data of the frame that follows it, so it
		// has to be serialised first and then sealed over - which means the
		// length field is written from a size the caller states rather than from
		// a payload it is holding. Append the sealed bytes to the same writer
		// afterwards and the result is exactly what `Write` would have produced.
		//
		// @param writer Where the bytes go.
		// @param header The header to write.
		// @param payloadBytes What the payload will be, sealed.
		// @return False when `payloadBytes` is over MAXIMUM_PAYLOAD_BYTES, in
		//         which case nothing is written.
		static bool WriteHeader(core::ByteWriter &writer, const PacketHeader &header, size_t payloadBytes);

		// What a successful read produced.
		struct Inbound {
			// The header, as sent.
			PacketHeader Header;

			// The header exactly as it arrived, HEADER_BYTES of it.
			//
			// **What to pass as associated data**, rather than re-serialising
			// the parsed fields. The two would agree today and the day they stop
			// agreeing is the day every packet is refused, which is a bug that
			// reads as a dead network.
			//
			// A view into the caller's buffer, like the payload.
			std::span<const std::byte> HeaderBytes;

			// The payload, as a view **into the caller's buffer**.
			//
			// Not copied. A packet is read and applied within one poll, and a
			// copy per packet per connection per tick is a per-frame allocation
			// this layer has no reason to make. The buffer has to outlive the
			// view, and it does: the poll owns it.
			//
			// Still sealed. Opening it is the caller's, because this class holds
			// no keys and must not.
			std::span<const std::byte> Payload;
		};

		// Reads a packet, refusing anything that is not one.
		//
		// Refuses a wrong magic, an unknown version, a channel that is not a
		// `ChannelKind`, a length that runs past the buffer, and a payload over
		// the maximum. Each is a `DisconnectReason::ProtocolError` at the call
		// site - not a warning, and not a partly filled `Inbound` a caller might
		// use.
		//
		// @param reader The bytes to parse.
		// @return The packet, or nothing. Nothing means drop it and count it.
		static std::optional<Inbound> Read(core::ByteReader &reader);

		// Which channel a datagram claims, without parsing the rest of it.
		//
		// For the router that has to decide where a datagram goes *before* it
		// has a connection to hand it to. A `ChannelKind::Handshake` datagram is
		// answered without a `Link` and everything else belongs to one, and on a
		// server the sender of the first kind is by definition not in the
		// connection table yet.
		//
		// Reads the magic, the version and the channel byte, and stops. **It is
		// not a substitute for `Read`** - nothing after the channel has been
		// looked at, so a caller that acts on the payload without reading it has
		// trusted a length nobody checked. It exists so that "which channel" is
		// one function rather than a byte offset copied into two routers.
		//
		// @param datagram The bytes as they arrived.
		// @return The channel, or nothing when this is not a packet this build
		//         reads.
		static std::optional<ChannelKind> PeekChannel(std::span<const std::byte> datagram);

		// Whether `sequence` is newer than `against`, accounting for wrapping.
		//
		// **A 16-bit counter wraps every 65536 packets**, which at sixty packets
		// a second is about eighteen minutes - well inside one session. A plain
		// `>` comparison therefore discards every packet for the eighteen minutes
		// after the first wrap, and a game that is fine in testing breaks in a
		// long match. The comparison is done on the half-range instead.
		//
		// @param sequence The sequence being judged.
		// @param against The newest sequence seen so far.
		// @return Whether `sequence` should be treated as more recent.
		static bool IsNewer(uint16_t sequence, uint16_t against);
	};
}
