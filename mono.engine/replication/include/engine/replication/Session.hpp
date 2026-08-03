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
// A snapshot chunk, an input and a structural change are reliable, because a
// lost chunk is a client that never joins, a lost input is a jump that never
// happened, and a lost creation is an entity the client never hears of again. A
// delta is unreliable, because the next one is already on its way and is more
// correct than the one being waited for — that is `net/AGENTS.md`'s rule about
// state against events, and putting the decision in the caller's hands is how
// somebody eventually makes everything reliable and turns one lost packet into
// a visible stall.
//
// **Every payload is sealed, and a session with no keys sends and accepts
// nothing.** `Admission.hpp`'s exchange ends with two directional ciphers and
// `AdoptKeys` is where they land; from that moment the payload of every packet
// is ChaCha20-Poly1305 over the plaintext with the packet header as associated
// data. There is no flag on the wire saying whether a packet is sealed and no
// call that sends one that is not — **a downgrade is only possible if a receiver
// can be asked for it, and nothing here can be asked.** A session that never
// adopted keys fails closed rather than falling back, because "in the clear just
// this once" is the whole of the attack.
//
// The one channel that stays clear is `net::ChannelKind::Handshake`, which is
// what establishes the keys and therefore cannot be sealed under them. It never
// passes through here at all: `Listener` and `Connector` frame it straight to
// the transport, and a handshake packet arriving at a session is refused before
// anything else looks at it.
//
// **Time is passed in, never read.** The same rule the two layers under this
// follow, and for the same reason: it makes a timeout something a suite states
// rather than waits for.
//
// @tier L12 · shared

#include <engine/core/Bytes.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Link.hpp>
#include <engine/net/Reliability.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <memory>
#include <optional>
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

		// Takes the two directional ciphers the admission exchange produced.
		//
		// Until this is called the session carries nothing in either direction.
		// The `Sealer` is held for the life of the connection, which is what
		// makes its counter monotone across every packet it will ever send —
		// **holding it is the nonce discipline, not a convenience.** It is
		// move-only and has no constructor from raw key material, so there is no
		// way for a second object to count from the same place under this key.
		//
		// **Not idempotent**, for the reason `net::Link::CompleteHandshake` is
		// not. A second set of keys on a live connection is a replay or two code
		// paths both believing they own the transition, and replacing a sealer
		// mid-connection is the one operation that could put a counter back to
		// zero under a key that has already used it.
		//
		// @param keys The ciphers, moved in. The sending half may already have
		//        sealed the exchange's own confirmation, and its counter carries
		//        on from there rather than restarting.
		// @return `false` when this session already holds keys, in which case
		//         the ones passed in are dropped and wiped.
		bool AdoptKeys(net::Handshake::Session keys);

		// Whether this session holds the keys to seal and open.
		//
		// @return `true` once `AdoptKeys` has succeeded.
		bool Sealing() const {
			return Sealer_.has_value();
		}

		// Queues a message for the peer.
		//
		// The channel is chosen from the message's kind — see the note at the
		// top of this file on why that is not the caller's decision.
		//
		// @param message    The encoded message.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it: over budget, closed, or
		//         holding no keys.
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
		// **The tag is checked before the link is told anything.** A packet that
		// does not open must not move the sequence window, retire a reliable
		// payload or reset the idle timeout, because all three are things
		// somebody who can write to this address could otherwise do to a
		// connection they have no key for.
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

			// Datagrams that were packets and did not open.
			//
			// **Apart from `Refused` on purpose**, the same way `Listener` keeps
			// its protocol refusals apart from its policy rejections. A number
			// climbing here is not a malformed sender: it is a forged or
			// tampered packet, a packet from a session that ended, or somebody
			// sending in the clear at a connection that has keys. An operator
			// reading one number for both cannot tell a broken client from
			// somebody injecting.
			uint64_t Unopened = 0;

			// Reliable payloads resent because they went unacknowledged.
			//
			// **Sealed again under a fresh counter, never replayed verbatim.**
			// See `Flush`.
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
		bool Transmit(net::PacketHeader header, std::span<const std::byte> payload);

		net::Transport *Transport_;
		net::Endpoint Peer_;
		net::Link Link_;
		net::ReliableSender Sender;
		net::ReliableReceiver Receiver;

		// The two halves of this connection's encryption, for its whole life.
		//
		// Empty until `AdoptKeys`, and everything refuses while they are — a
		// session that cannot seal must not send, and one that cannot open must
		// not accept. `std::optional` rather than a pointer because the ciphers
		// are move-only and this is the only thing that owns them; a copy of a
		// `Sealer` is exactly what the type refuses to allow.
		std::optional<net::Cipher::Sealer> Sealer_;
		std::optional<net::Cipher::Opener> Opener_;

		std::vector<std::vector<std::byte>> Inbound_;

		// The datagram being framed, reused across ticks so a session polled
		// every frame stops allocating. It holds the header while the header is
		// being sealed over and the sealed frame afterwards.
		core::ByteWriter Framing;

		Statistics Stats_;
	};

	// Which channel a message kind travels on.
	//
	// Reliable for the things whose loss is visible as an absence — a snapshot
	// chunk, an input, a structural change — and unreliable for the one whose
	// loss is covered by the next one arriving.
	//
	// @param kind The message kind.
	// @return The channel it belongs on.
	// @since v0.3
	net::ChannelKind ChannelFor(MessageKind kind);
}
