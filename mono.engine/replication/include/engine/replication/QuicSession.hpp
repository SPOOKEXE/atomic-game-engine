#pragma once

// The QUIC half of `SessionPort`, and the four members it does not have.
//
// `docs/CODE_ARCH.md` §10.1: *"`Session` owns a `net::Link`, a `ReliableSender`,
// a `ReliableReceiver` and its `Sealer`/`Opener` pair as members. Those are
// exactly the pieces QUIC would supply itself - streams, acknowledgement, and
// TLS 1.3 - so a QUIC session is not 'a `Transport` with a different `Send`', it
// is a session that skips four of its own members."* This is that session. It
// owns a `quic::Connection` and nothing else that carries bytes.
//
// **What it keeps from the old design is the ceiling, and only the ceiling.**
// `LinkSettings::BytesPerTick` survives *above* ngtcp2's controller,
// because the two answer different questions. A game may refuse to
// spend more than N on one player on a path that would carry ten times that,
// since a hundred players on one host is a hundred of these and the operator's
// bill is not a function of what the path can take. So there is a per-tick byte
// budget here, reset at the barrier with everything else, and the pacing
// underneath it is the transport's.
//
// **A message's channel is its kind, and the mapping is `docs/CODE_ARCH.md`
// §10's table read literally.** The join snapshot, structure, inputs, applied
// state, user messages and the identity claim each get a stream of their own,
// which is the whole point: they share one reliable window today, so a megabyte
// snapshot stalls a door opening and §10 calls that "a property of the design".
// Deltas, group signatures and audit answers travel as RFC 9221 datagrams,
// unreliable and acknowledged.
//
// @tier L12 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/quic/Connection.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/SessionPort.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace engine::replication {

	// How a QUIC session is configured.
	//
	// @since v0.19
	struct QuicSessionSettings {
		// The connection's own settings - the identity, the flow-control limits
		// and the idle timeout.
		net::quic::ConnectionSettings Connection;

		// The hard ceiling on what one peer costs per tick, in bytes.
		//
		// **Above the congestion controller rather than instead of it.** See the
		// note at the top of this file.
		uint32_t BytesPerTick = 64 * 1024;

		// How long a session may sit queued before the delay simulation lets it
		// go, in milliseconds. Zero is off.
		double SimulatedLatencyMilliseconds = 0.0;
	};

	// Which channel a message kind travels on under QUIC, and how.
	//
	// @since v0.19
	struct QuicRoute {
		// The channel number, below `net::quic::MAXIMUM_CHANNELS`.
		uint8_t Channel = 0;

		// Whether it goes on a stream or as a datagram.
		bool Reliable = true;
	};

	// Which channel a message kind travels on under QUIC.
	//
	// **One stream per kind rather than one per reliability class**, which is
	// the difference between this and `ChannelFor`. Two kinds sharing a stream
	// share its ordering, and the pair that most obviously must not is a
	// snapshot chunk and a structural change.
	//
	// @param kind The message kind.
	// @return The channel and whether it is reliable.
	// @since v0.19
	QuicRoute QuicRouteFor(MessageKind kind);

	// One QUIC connection, carrying replication messages both ways.
	//
	// @since v0.19
	class QuicSession final : public SessionPort {
	  public:
		// Opens a session to a server.
		//
		// @param transport  The wire. Borrowed, not owned.
		// @param peer       Where the server is.
		// @param nowSeconds The current time.
		// @param settings   How to configure it.
		// @return The session, or nothing when the connection could not be made.
		static std::unique_ptr<QuicSession> Connect(
			net::Transport &transport,
			const net::Endpoint &peer,
			double nowSeconds,
			const QuicSessionSettings &settings
		);

		// Answers a client's first packet.
		//
		// @param transport  The wire.
		// @param peer       Where the client is.
		// @param datagram   The client's first datagram, whole.
		// @param nowSeconds The current time.
		// @param settings   How to configure it. Needs an identity seed.
		// @return The session, or nothing when the datagram does not open one.
		static std::unique_ptr<QuicSession> Accept(
			net::Transport &transport,
			const net::Endpoint &peer,
			std::span<const std::byte> datagram,
			double nowSeconds,
			const QuicSessionSettings &settings
		);

		~QuicSession() override;

		const net::Endpoint &Peer() const override;
		bool Send(std::span<const std::byte> message, double nowSeconds) override;
		size_t Flush(double nowSeconds) override;
		bool Receive(std::span<const std::byte> datagram, double nowSeconds) override;
		void ClearInbound() override;
		void Advance(double nowSeconds) override;
		bool Carrying() const override;
		bool Live() const override;
		void Disconnect(double nowSeconds) override;
		float RoundTripMilliseconds() const override;
		size_t SendAllowanceBytes() const override;
		void SetSimulatedLatency(double milliseconds) override;
		double SimulatedLatency() const override;
		bool Binding(std::span<std::byte> out) const override;

		// The messages that have arrived and not yet been taken.
		//
		// @return The messages.
		std::span<const std::vector<std::byte>> Inbound() const override {
			return Arrived;
		}

		// The connection underneath.
		//
		// **Narrower than it looks.** What a caller needs it for is the two
		// things a session cannot answer - which connection ids this end
		// answers to, so a listener can route a datagram, and who the peer
		// proved itself to be. Everything a session *does* is on `SessionPort`.
		//
		// @return The connection.
		const net::quic::Connection &Wire() const {
			return *Connection_;
		}

		// What this session has done.
		//
		// @return The connection's statistics.
		net::quic::Connection::Statistics Stats() const;

		// Whether the far end said it does not speak QUIC.
		//
		// **Not on `SessionPort`, deliberately.** The datagram session has no
		// answer to give: its refusal is an `AdmissionKind::Refuse` message the
		// connector reads itself, not a state the session holds. A virtual that
		// one implementation could only ever answer `false` would be a widening
		// of the interface to save a caller one cast.
		//
		// @return `true` once a Version Negotiation packet has arrived.
		// @since v0.19
		bool Refused() const;

	  private:
		QuicSession(std::unique_ptr<net::quic::Connection> connection, const QuicSessionSettings &settings);

		void Take(double nowSeconds);

		std::unique_ptr<net::quic::Connection> Connection_;
		QuicSessionSettings Settings;

		std::vector<std::vector<std::byte>> Arrived;

		// What is left of this tick's byte ceiling. See the note at the top.
		uint32_t Remaining = 0;

		double SimulatedLatencySeconds = 0.0;

		// Messages held back by the delay simulation, with the time each is due.
		//
		// Held *before* the connection rather than after it, which is the one
		// difference from `Session`'s queue: QUIC frames a packet when it is
		// sent, so a datagram delayed after framing would carry a stale
		// acknowledgement and a packet number out of step with its neighbours.
		struct Delayed {
			double ReadyAtSeconds = 0.0;
			std::vector<std::byte> Bytes;
		};

		std::deque<Delayed> Waiting;
	};
}
