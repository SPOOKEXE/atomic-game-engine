#pragma once

// The named states a connection talks in.
//
// Every one of these crosses a boundary - a client reads why it was dropped, a
// log records a lifecycle change, a script asks a `ConnectionStats` what channel
// something went on - so each is a type rather than a bool or an int that loses
// its meaning at the first hop.
//
// **Names are the format, numbers are not.** Anything written to a log carries
// the name. The wire is the exception and says so where it happens: a packet
// header has one byte for a channel, because a wire that spells out
// "unreliable" in every packet is a wire nobody would ship.
//
// @tier L11 · shared

#include <cstdint>

namespace engine::net {

	// Where a connection is in its life.
	//
	// A strict progression: nothing goes backwards, and a connection that has
	// reached Disconnected stays there. Reconnecting is a *new* connection with
	// a new id, not a revived one - a handle that can come back to life is a
	// handle every caller has to re-check after every await, and that check is
	// the one nobody writes.
	//
	// @since v0.3
	enum class ConnectionState : uint8_t {
		// A handshake is in flight. No payload may be sent yet, because there
		// is nothing to say who the far side is.
		Connecting,

		// Established. Payload flows in both directions.
		Connected,

		// A disconnect has been decided and the reason has yet to reach the far
		// side. Payload is refused, but the connection is still polled - this
		// state exists so a goodbye arrives rather than the far side waiting out
		// a timeout for a peer that left politely.
		Disconnecting,

		// Over. The id is dead and will not be reused within its generation.
		Disconnected,
	};

	// Why a connection ended.
	//
	// Kept apart from the state because "it is over" and "here is what happened"
	// are different questions, and a client that shows a player one message for
	// all four is a client nobody can support.
	//
	// @since v0.3
	enum class DisconnectReason : uint8_t {
		// Still connected, or never connected. The default, so a partly filled
		// record does not read as a graceful close.
		None,

		// Either side called Disconnect and said goodbye.
		Requested,

		// Nothing arrived within the timeout. The far side crashed, was killed,
		// or is behind a network that stopped.
		TimedOut,

		// The handshake did not complete. A wrong protocol version, a full
		// server, or a peer that never answered.
		HandshakeFailed,

		// A packet arrived that this build refuses to parse: a bad magic, an
		// unknown version, a length that contradicts the frame.
		//
		// **Never treated as corruption to be retried.** repo_layout.md §1 says
		// anyone can run a server, so a malformed packet is as likely to be an
		// attempt as an accident, and retrying is exactly what an attacker wants.
		ProtocolError,

		// The far side exceeded a budget - bytes per tick, packets per tick, or
		// a payload larger than a channel allows.
		BudgetExceeded,

		// The local process is going away.
		Shutdown,
	};

	// How much the transport promises about a payload.
	//
	// **Unreliable is the default and that is a design commitment**, not an
	// omission - DATATYPES_LIBRARIES.md §15.1. A late position update is worse
	// than a dropped one, because the next one is already on its way and is more
	// correct than the one being waited for. Making everything reliable is the
	// mistake that turns one lost packet into a visible stall for every player.
	//
	// @since v0.3
	enum class ChannelKind : uint8_t {
		// State. Dropped rather than resent, and delivered out of order.
		//
		// A receiver discards anything older than what it already has, which is
		// why a sequence number rides every packet.
		Unreliable,

		// Events. Resent until acknowledged and delivered in order.
		//
		// For the things a game cannot lose and cannot reorder: a door opened, a
		// player joined, a script's remote call.
		Reliable,

		// Connection setup, before there is a connection.
		//
		// **Neither of the two above, and that is why it is a channel rather
		// than a payload byte.** A handshake datagram is answered without a
		// `Link` - the whole point is that a stranger's first datagram allocates
		// nothing - so there is no sequence for it to be newer than and no
		// reliable window for it to be ordered in. Marking it here means it is
		// still a `Packet`: one magic, one version, one framing, and a router
		// can tell "this is somebody trying to connect" from "this belongs to a
		// connection" before it has decided whether the sender is either.
		//
		// Retransmission on this channel is the *initiator's*, on a timer, and
		// nothing here holds anything for it. A responder that had to remember
		// what it sent is exactly the state the challenge exists to avoid.
		Handshake,
	};

	// Returns a stable, human-readable name for a connection state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ConnectionState state);

	// Returns a stable, human-readable name for a disconnect reason.
	//
	// @param reason The reason to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(DisconnectReason reason);

	// Returns a stable, human-readable name for a channel kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ChannelKind kind);
}
