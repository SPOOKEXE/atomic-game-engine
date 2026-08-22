#pragma once

// What a session has to be, so that there can be two of them.
//
// `docs/CODE_ARCH.md` §10.1 asks the first design question of the QUIC work and
// leaves it open: *"Deciding whether that is a second `Session` implementation
// behind a new interface, or a `Session` whose reliability pieces become
// optional, is the first design question of the work rather than the last."*
//
// **This is the answer, and it is the first of the two.** A `Session` whose
// `ReliableSender`, `ReliableReceiver`, `Sealer` and `Opener` become optional is
// one class with two modes, and every method on it grows a branch that only one
// configuration exercises - which is the same argument `docs/QUIC.md` §8 makes
// about the tree as a whole ("two overlapping reliability stacks is worse than
// either") applied inside a single type. The four members are not incidental
// state; they *are* the design, and a QUIC session does not have a different
// version of them, it has none.
//
// So there are two implementations and one interface, and what the interface
// contains is exactly what a caller above the transport does: queue a message,
// take what arrived, turn the tick over, and ask whether the link is still
// there. What it deliberately does **not** contain:
//
// - **Anything about keys.** `Session::AdoptKeys` exists because the admission
//   exchange happens outside the session and hands the ciphers in; QUIC's keys
//   are the handshake's own and never surface. A port with an `AdoptKeys` on it
//   would have one implementation refusing it forever.
// - **`net::Link`.** A caller reaching through a session to a link is a caller
//   coupled to one reliability design. The three things every consumer actually
//   wanted from it - the round trip, the send allowance, whether the link is
//   still up - are named here instead.
// - **Anything about channels.** Which channel a message travels on is a
//   function of its kind, and both implementations work it out from the same
//   `MessageKind`. A caller choosing would be a caller that can drift.
//
// @tier L12 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Wire.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::replication {

	// One connection, carrying replication messages both ways.
	//
	// @since v0.19
	class SessionPort {
	  public:
		virtual ~SessionPort() = default;

		// The peer this talks to.
		//
		// @return Its address.
		virtual const net::Endpoint &Peer() const = 0;

		// Queues a message for the peer.
		//
		// The channel is chosen from the message's kind, never by the caller.
		//
		// @param message    The encoded message.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it: over budget, closed, or not
		//         yet able to carry anything.
		virtual bool Send(std::span<const std::byte> message, double nowSeconds) = 0;

		// Sends what is queued and repeats what has gone unanswered.
		//
		// @param nowSeconds The current time.
		// @return The number of datagrams handed to the transport.
		virtual size_t Flush(double nowSeconds) = 0;

		// Takes one datagram that arrived from this peer.
		//
		// **Every field of it is hostile.**
		//
		// @param datagram   The bytes as they arrived.
		// @param nowSeconds The current time.
		// @return `false` when it was refused.
		virtual bool Receive(std::span<const std::byte> datagram, double nowSeconds) = 0;

		// The messages that have arrived and not yet been taken.
		//
		// @return The messages.
		virtual std::span<const std::vector<std::byte>> Inbound() const = 0;

		// Drops the arrived messages, once the caller has applied them.
		virtual void ClearInbound() = 0;

		// Turns the tick over: ages timers and resets the per-tick budget.
		//
		// **One call rather than the two a caller used to make**, because
		// `Advance` and `ResetBudget` were never usefully separate and a caller
		// that made one and not the other spent two ticks' worth inside one.
		//
		// @param nowSeconds The current time.
		virtual void Advance(double nowSeconds) = 0;

		// Whether this session can carry a message right now.
		//
		// @return `true` once the handshake is behind it.
		virtual bool Carrying() const = 0;

		// Whether this session still exists at all.
		//
		// @return `false` once it has ended, at which point it stays ended - a
		//         reconnect is a new session, never a revived one.
		virtual bool Live() const = 0;

		// Says goodbye.
		//
		// **Not a formality**, for the reason `net/AGENTS.md` gives: a peer that
		// left politely must be distinguishable from one that crashed, or every
		// clean exit costs the other end a full idle timeout.
		//
		// @param nowSeconds The current time.
		virtual void Disconnect(double nowSeconds) = 0;

		// The round-trip estimate.
		//
		// @return The estimate in milliseconds. Zero means unknown, not instant.
		virtual float RoundTripMilliseconds() const = 0;

		// How many bytes this session will carry this tick.
		//
		// What `Authority::SetAllowance` is given, so that a tick's worth of
		// world state is sized against what the path and the operator's ceiling
		// will actually take.
		//
		// @return The allowance in bytes.
		virtual size_t SendAllowanceBytes() const = 0;

		// Adds deterministic one-way delay before datagrams reach the transport.
		//
		// @param milliseconds The added delay.
		virtual void SetSimulatedLatency(double milliseconds) = 0;

		// The effective delay after sanitising.
		//
		// @return The delay in milliseconds.
		virtual double SimulatedLatency() const = 0;

		// A value both ends of this session compute and nobody else can.
		//
		// What a client's identity claim is signed over, so that a signature
		// captured from one session proves nothing on another and a relay
		// holding one exchange with each side cannot carry the claim across.
		// The datagram session's is its admission transcript; QUIC's is a TLS
		// exporter.
		//
		// @param out Where the binding goes.
		// @return `false` before there is a session to bind to.
		virtual bool Binding(std::span<std::byte> out) const = 0;
	};
}
