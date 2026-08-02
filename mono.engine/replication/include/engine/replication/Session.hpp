#pragma once

// One peer's connection, from the wire up to a replication message.
//
// **This is the joint.** `net` gives a `Transport` that moves datagrams, a
// `Link` that owns the lifecycle and the acknowledgement window, and a
// `ReliableSender`/`ReliableReceiver` pair that turns a lossy channel into an
// ordered one. `replication` gives messages. Nothing above knew how to put them
// together and nothing below was allowed to, because `net` must not know what a
// component is — so the joint is here, once, rather than in the server and
// again in the client.
//
// **Which channel a message goes on is decided here and not by the caller.**
// A snapshot chunk and an input are reliable, because a lost chunk is a client
// that never joins and a lost input is a jump that never happened. A delta is
// unreliable, because the next one is already on its way and is more correct
// than the one being waited for — that is `net/AGENTS.md`'s rule about state
// against events, and putting the decision in the caller's hands is how
// somebody eventually makes everything reliable and turns one lost packet into
// a visible stall.
//
// **Time is passed in, never read.** The same rule the two layers under this
// follow, and for the same reason: it makes a timeout something a suite states
// rather than waits for.
//
// @tier L12 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Link.hpp>
#include <engine/net/Reliability.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::replication {

	// How a session frames and resends.
	//
	// @since v0.3
	struct SessionSettings {
		// The link's own settings — budgets, timeouts, keep-alive.
		net::LinkSettings Link;

		// How much unacknowledged reliable traffic is held.
		net::ReliabilitySettings Reliability;
	};

	// One connection, carrying replication messages both ways.
	//
	// @since v0.3
	class Session {
	  public:
		// Opens a session over a transport, to one peer.
		//
		// @param transport   The wire. Borrowed, not owned: one socket serves
		//                    every peer a server has, and a session per socket
		//                    would be a port per player.
		// @param peer        Where the other end is.
		// @param id          The connection's handle.
		// @param nowSeconds  The current time.
		// @param settings    How to frame and resend.
		Session(
			net::Transport &transport,
			const net::Endpoint &peer,
			net::ConnectionId id,
			double nowSeconds,
			const SessionSettings &settings = {}
		);

		// The peer this talks to.
		//
		// @return Its address.
		const net::Endpoint &Peer() const {
			return Peer_;
		}

		// The connection state machine.
		//
		// @return The link.
		net::Link &Link() {
			return Link_;
		}

		// The connection state machine.
		//
		// @return The link.
		const net::Link &Link() const {
			return Link_;
		}

		// Queues a message for the peer.
		//
		// The channel is chosen from the message's kind — see the note at the
		// top of this file on why that is not the caller's decision.
		//
		// @param message    The encoded message.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it: over budget, or closed.
		bool Send(std::span<const std::byte> message, double nowSeconds);

		// Sends what is queued and resends what has gone unacknowledged.
		//
		// @param nowSeconds The current time.
		// @return The number of datagrams handed to the transport.
		size_t Flush(double nowSeconds);

		// Takes one datagram that arrived from this peer.
		//
		// **Every field of it is hostile.** A datagram that is not a packet, or
		// is a packet for a channel this does not run, is refused and counted;
		// it is never partly applied.
		//
		// @param datagram   The bytes as they arrived.
		// @param nowSeconds The current time.
		// @return `false` when it was refused.
		bool Receive(std::span<const std::byte> datagram, double nowSeconds);

		// The messages that have arrived and not yet been taken.
		//
		// Reliable ones are in the order they were sent, with a gap held until
		// it is filled. Unreliable ones are in the order they arrived, minus
		// anything already superseded.
		//
		// @return The messages.
		std::span<const std::vector<std::byte>> Inbound() const {
			return Inbound_;
		}

		// Drops the arrived messages, once the caller has applied them.
		void ClearInbound();

		// What this session has done.
		//
		// @since v0.3
		struct Statistics {
			// Datagrams handed to the transport.
			uint64_t Sent = 0;

			// Datagrams the transport refused — full, closed or unreachable.
			uint64_t Undeliverable = 0;

			// Datagrams refused as not being a packet for this session.
			uint64_t Refused = 0;

			// Reliable payloads resent because they went unacknowledged.
			uint64_t Retransmissions = 0;
		};

		// What this session has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		bool Emit(net::ChannelKind channel, std::span<const std::byte> payload, double nowSeconds);

		net::Transport *Transport_;
		net::Endpoint Peer_;
		net::Link Link_;
		net::ReliableSender Sender;
		net::ReliableReceiver Receiver;

		std::vector<std::vector<std::byte>> Inbound_;

		// Reused across ticks so a session polled every frame stops
		// allocating.
		std::vector<std::byte> Scratch;

		Statistics Stats_;
	};

	// Which channel a message kind travels on.
	//
	// Reliable for the things whose loss is visible as an absence — a snapshot
	// chunk, an input, a forget — and unreliable for the one whose loss is
	// covered by the next one arriving.
	//
	// @param kind The message kind.
	// @return The channel it belongs on.
	// @since v0.3
	net::ChannelKind ChannelFor(MessageKind kind);
}
