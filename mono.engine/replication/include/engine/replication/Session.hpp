#pragma once

// Owns framing, channel selection, reliability, and encryption for one peer.
// Payloads require adopted keys; handshake packets bypass this class.
// Message kind selects reliable or unreliable delivery so callers cannot drift.
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
#include <engine/replication/SessionPort.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// How a session frames and resends.
	//
	// @since v0.3
	struct SessionSettings {
		// The link's own settings - budgets, timeouts, keep-alive.
		net::LinkSettings Link;

		// How much unacknowledged reliable traffic is held.
		net::ReliabilitySettings Reliability;
	};

	// One connection, carrying replication messages both ways.
	//
	// **The datagram half of `SessionPort`**, and the one this engine has had
	// since v0.3: `net::Packet`'s framing, one shared reliable window, the
	// X25519 exchange and a `Sealer`/`Opener` pair. `SessionPort.hpp` is the
	// argument for why the QUIC one is a second class rather than a mode of this
	// one.
	//
	// @since v0.3
	class Session final : public SessionPort {
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
		const net::Endpoint &Peer() const override {
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
		// makes its counter monotone across every packet it will ever send -
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
		// The channel is chosen from the message's kind - see the note at the
		// top of this file on why that is not the caller's decision.
		//
		// @param message    The encoded message.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it: over budget, closed, or
		//         holding no keys.
		bool Send(std::span<const std::byte> message, double nowSeconds) override;

		// Adds deterministic one-way delay before sealed datagrams reach the
		// transport. Already queued datagrams keep the deadline they received.
		//
		// Non-finite and negative values become zero. Values above one minute are
		// capped so a mistaken property cannot retain an unbounded queue.
		//
		// @param milliseconds The added delay.
		// @since v0.18
		void SetSimulatedLatency(double milliseconds) override;

		// The effective delay after sanitising, in milliseconds.
		double SimulatedLatency() const override {
			return SimulatedLatencySeconds * 1000.0;
		}

		// Sends what is queued and resends what has gone unacknowledged.
		//
		// @param nowSeconds The current time.
		// @return The number of datagrams handed to the transport.
		size_t Flush(double nowSeconds) override;

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
		bool Receive(std::span<const std::byte> datagram, double nowSeconds) override;

		// The messages that have arrived and not yet been taken.
		//
		// Reliable ones are in the order they were sent, with a gap held until
		// it is filled. Unreliable ones are in the order they arrived, minus
		// anything already superseded.
		//
		// @return The messages.
		std::span<const std::vector<std::byte>> Inbound() const override {
			return Inbound_;
		}

		// Drops the arrived messages, once the caller has applied them.
		void ClearInbound() override;

		// Turns the tick over: ages the link's timers and resets its budget.
		//
		// @param nowSeconds The current time.
		// @since v0.19
		void Advance(double nowSeconds) override;

		// Whether this session can carry a message right now.
		//
		// @return `true` once it holds keys.
		// @since v0.19
		bool Carrying() const override {
			return Sealer_.has_value();
		}

		// Whether this session still exists at all.
		//
		// @return `false` once the link is `Disconnected`.
		// @since v0.19
		bool Live() const override;

		// Says goodbye.
		//
		// @param nowSeconds The current time.
		// @since v0.19
		void Disconnect(double nowSeconds) override;

		// The round-trip estimate the reliable sender measured.
		//
		// @return The estimate in milliseconds. Zero means unknown.
		// @since v0.19
		float RoundTripMilliseconds() const override;

		// How many bytes this session will carry this tick.
		//
		// @return `net::Link`'s allowance, which is Copa's window under
		//         `BytesPerTick`.
		// @since v0.19
		size_t SendAllowanceBytes() const override;

		// Records the admission transcript this session is bound to.
		//
		// **Filled by whoever ran the exchange**, because a session does not run
		// one: `Listener::Accept` and `Connector` both build the transcript from
		// the two public keys and the cookie, and this is where it is kept so
		// that an identity claim has something to be checked against.
		//
		// @param binding The transcript.
		// @since v0.19
		void SetBinding(std::span<const std::byte> binding);

		// The admission transcript this session is bound to.
		//
		// @param out Where it goes. Must match the transcript's length.
		// @return `false` when nothing has been recorded, or the lengths differ.
		// @since v0.19
		bool Binding(std::span<std::byte> out) const override;

		// What this session has done.
		//
		// @since v0.3
		struct Statistics {
			// Datagrams handed to the transport.
			uint64_t Sent = 0;

			// Datagrams the transport refused - full, closed or unreachable.
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

			// Packets sent carrying nothing but an acknowledgement.
			//
			// **The number that says a quiet link is still a link.** A session
			// that carries occasional messages - the studio's edit stream -
			// spends most of its life here, and a zero on a session that has
			// been idle means acknowledgements are not flowing.
			//
			// @since v0.13
			uint64_t KeepAlives = 0;
		};

		// What this session has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		bool Emit(net::ChannelKind channel, std::span<const std::byte> payload, double nowSeconds);
		bool Transmit(net::PacketHeader header, std::span<const std::byte> payload, double nowSeconds);
		size_t FlushDelayed(double nowSeconds);

		struct DelayedDatagram {
			double ReadyAtSeconds = 0.0;
			std::vector<std::byte> Bytes;
		};

		net::Transport *Transport_;
		net::Endpoint Peer_;
		net::Link Link_;
		net::ReliableSender Sender;
		net::ReliableReceiver Receiver;

		// Whether a reliable payload has been accepted and not yet
		// acknowledged. See `Flush`.
		//
		// **A flag rather than a count**, because one packet acknowledges
		// everything received up to it - the header carries a sequence and a
		// bitfield, not a list.
		bool Owed = false;

		// The two halves of this connection's encryption, for its whole life.
		//
		// Empty until `AdoptKeys`, and everything refuses while they are - a
		// session that cannot seal must not send, and one that cannot open must
		// not accept. `std::optional` rather than a pointer because the ciphers
		// are move-only and this is the only thing that owns them; a copy of a
		// `Sealer` is exactly what the type refuses to allow.
		std::optional<net::Cipher::Sealer> Sealer_;
		std::optional<net::Cipher::Opener> Opener_;

		std::vector<std::vector<std::byte>> Inbound_;

		// The admission transcript, recorded by whoever ran the exchange. See
		// `SetBinding`.
		std::vector<std::byte> Binding_;

		// The datagram being framed, reused across ticks so a session polled
		// every frame stops allocating. It holds the header while the header is
		// being sealed over and the sealed frame afterwards.
		core::ByteWriter Framing;

		// Sealed before queuing, exactly like bytes already in flight on a real
		// network. The FIFO preserves send order for equal and increasing delays.
		std::deque<DelayedDatagram> Delayed;
		double SimulatedLatencySeconds = 0.0;

		Statistics Stats_;
	};

	// Which channel a message kind travels on.
	//
	// Reliable for the things whose loss is visible as an absence - a snapshot
	// chunk, an input, a structural change - and unreliable for the one whose
	// loss is covered by the next one arriving.
	//
	// @param kind The message kind.
	// @return The channel it belongs on.
	// @since v0.3
	net::ChannelKind ChannelFor(MessageKind kind);
}
