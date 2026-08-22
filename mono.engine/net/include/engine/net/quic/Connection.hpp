#pragma once

// One QUIC connection, over the `Transport` this module already had.
//
// **QUIC is a UDP payload, so the transport survives the swap.** `Transport.hpp`
// carries datagrams and holds no connection state; everything a connection is -
// the lifecycle, the packet numbers, the acknowledgements, the streams - lives
// here, and the loopback and the socket are as interchangeable underneath this
// as they are underneath `Link`. That is what lets a QUIC connection be proved
// over `LossyTransport`, which is how the hand-rolled stack was proved.
//
// The transport is ngtcp2. The crypto is `quic/Crypto.hpp` and the handshake is
// `quic/Tls.hpp`, both first-party, both reached through a callback table that
// never leaves this file.
//
// **What this replaces from the module around it**, and it is most of it:
// `Packet`'s framing, `Reliability`'s sender and receiver, `Handshake`'s X25519
// exchange, `Cipher`'s sealing, `Cookie`'s challenge and `ConnectionId`. QUIC
// supplies every one, interoperably tested, with per-stream loss recovery that
// the hand-rolled window structurally cannot have. What it does **not** replace
// is `Link::BytesPerTick` and `CongestionControl`, and `docs/QUIC.md` §6 and §9
// are why: a per-player byte ceiling answers a question about an operator's
// bill rather than about a path, and Copa is a delay-based controller tuned for
// input latency where ngtcp2's default is Cubic.
//
// ## Channels are sender-owned unidirectional streams
//
// A caller names a channel by a small number and this end opens **its own**
// unidirectional stream for it, once, lazily. Three consequences, and each is
// the reason rather than a side effect:
//
// - **One stream per channel is what buys the head-of-line isolation.** The old
//   design shares one reliable window across structure, inputs, RPC and the
//   join snapshot, so a megabyte snapshot stalls a door opening.
//   `docs/CODE_ARCH.md` §10 calls that "a property of the design", and this is
//   the mechanism that removes it.
// - **Unidirectional rather than bidirectional**, because a channel is a
//   direction. A bidirectional stream would make one end's ordering and one
//   end's flow control the same object as the other's, and a reader stalling
//   would then push back on a writer that has nothing to do with it.
// - **The channel number is the first byte of the stream**, sent once. Deriving
//   it from the stream id instead would work only while both ends open their
//   streams in the same order and never skip one, which is a coupling nothing
//   would report the breaking of.
//
// Messages on a stream are length-framed, because a QUIC stream is bytes and a
// caller above sends messages. Unreliable traffic is RFC 9221 DATAGRAM frames,
// one message each, with the same channel byte in front - never retransmitted,
// sharing the connection's congestion controller, and **acknowledged**, which
// is the delivery signal `D00014` says the hand-rolled stack cannot give itself
// without inventing a second acknowledgement path.
//
// ## Time is passed in, never read
//
// Every entry point takes `nowSeconds`, exactly as `Link` does, and ngtcp2 was
// chosen partly because every one of *its* entry points takes an explicit
// timestamp. There is no timer thread and no clock read: the retransmission
// timer, the idle timeout and the loss detection are all driven by `Flush`
// being called with the tick's time, and `ExpirySeconds` is how a caller knows
// when the next one matters.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/quic/Tls.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::net::quic {

	// How long a connection id this end hands out.
	//
	// Eight bytes, which is what the protocol's own short header assumes when a
	// receiver must know the length without being told. Longer would cost the
	// difference on every packet in both directions for a collision space
	// already far past what one host will hold.
	inline constexpr size_t CONNECTION_ID_BYTES = 8;

	// The largest channel number a caller may name.
	//
	// Sixteen streams each way, which is four more than every channel this
	// engine has an argument for. The cap exists because a channel byte from the
	// wire indexes an array and a peer chooses it.
	inline constexpr uint8_t MAXIMUM_CHANNELS = 16;

	// What a connection is doing.
	//
	// The same one-way lifecycle `net/AGENTS.md` insists on for `Link`: nothing
	// goes backwards, and a `Closed` connection stays closed. A reconnect is a
	// new connection, never a revived one.
	//
	// @since v0.19
	enum class ConnectionState : uint8_t {
		// The handshake is in flight. Nothing a caller sent has gone anywhere
		// yet and nothing it receives is authenticated.
		Handshaking,

		// The handshake completed, both ends are authenticated, and traffic
		// flows.
		Established,

		// This end has sent a CONNECTION_CLOSE and is repeating it for anything
		// still in flight. RFC 9000 §10.2.
		Closing,

		// Nothing more will be sent or received.
		Closed,
	};

	// Returns a stable, human-readable name for a connection state.
	//
	// @param state The state.
	// @return A view valid for the lifetime of the process.
	// @since v0.19
	const char *Describe(ConnectionState state);

	// How a connection is configured, in the units a caller thinks in.
	//
	// @since v0.19
	struct ConnectionSettings {
		// The handshake's identity, protocol and pinning.
		TlsSettings Tls;

		// How many bytes of unread stream data the peer may have outstanding
		// across the whole connection.
		uint64_t MaximumConnectionData = 4 * 1024 * 1024;

		// How many the peer may have outstanding on any one stream.
		//
		// **Below the connection limit on purpose.** One channel filling the
		// whole window is the head-of-line stall the streams were opened to
		// remove, arriving through flow control instead of through ordering.
		uint64_t MaximumStreamData = 1024 * 1024;

		// The largest message a caller may send or accept on a stream.
		//
		// Every inbound length is checked against this before a byte is
		// reserved, because the length is the peer's and an allocation sized
		// from it is the peer's too.
		uint32_t MaximumMessageBytes = 1024 * 1024;

		// How long a silent connection lives.
		//
		// Both ends state one and RFC 9000 §10.1 takes the smaller, so a peer
		// cannot hold a slot open by asking for a longer one.
		uint64_t IdleTimeoutMilliseconds = 20000;

		// Whether unreliable messages are offered at all.
		//
		// Off means no `max_datagram_frame_size` in the transport parameters,
		// which is how RFC 9221 spells "do not send me any" - and
		// `SendUnreliable` then refuses rather than silently sending nothing.
		bool AllowUnreliable = true;
	};

	// What arrived from the peer.
	//
	// @since v0.19
	struct Arrival {
		// Which channel it came on.
		uint8_t Channel = 0;

		// Whether it came reliably. A caller that treats the two the same is a
		// caller that did not need two.
		bool Reliable = true;

		// The message.
		std::vector<std::byte> Bytes;
	};

	// One end of a QUIC connection.
	//
	// Not copyable and not movable: ngtcp2 holds a pointer back to this object
	// for every callback it makes, so an address that moved is a callback into
	// nothing. Held behind a `unique_ptr`, which is what the two factory
	// functions return.
	//
	// @since v0.19
	class Connection {
	  public:
		// Opens a connection to a server.
		//
		// Nothing is sent by this call. The first datagram goes out on the first
		// `Flush`, which is what keeps the "no I/O outside a named point" shape
		// the rest of this module has.
		//
		// @param transport  The wire. Borrowed, not owned: one socket serves
		//        every peer a server has.
		// @param peer       Where the server is.
		// @param nowSeconds The current time.
		// @param settings   How to configure it.
		// @return The connection, or nothing when the settings are unusable or
		//         the operating system refused entropy.
		static std::unique_ptr<Connection>
		Connect(Transport &transport, const Endpoint &peer, double nowSeconds, ConnectionSettings settings);

		// Answers a client's first packet.
		//
		// **The caller decides whether to answer at all**, and that decision is
		// above this module - `replication::Listener::SetAdmission` is where it
		// lives. What this does is take a datagram that `Accepts` said is a
		// client's opening Initial and stand a connection up from it.
		//
		// @param transport  The wire.
		// @param peer       Where the client is.
		// @param datagram   The client's first datagram, whole.
		// @param nowSeconds The current time.
		// @param settings   How to configure it. Needs an identity seed.
		// @return The connection, or nothing when the datagram is not an
		//         acceptable Initial packet.
		static std::unique_ptr<Connection> Accept(
			Transport &transport,
			const Endpoint &peer,
			std::span<const std::byte> datagram,
			double nowSeconds,
			ConnectionSettings settings
		);

		~Connection();

		Connection(const Connection &) = delete;
		Connection &operator=(const Connection &) = delete;
		Connection(Connection &&) = delete;
		Connection &operator=(Connection &&) = delete;

		// Takes one datagram that arrived from this peer.
		//
		// **Every field of it is hostile**, and QUIC's own protections do most of
		// the work: a packet that does not open under the keys for its level is
		// discarded before anything it claims is believed, which covers the
		// sequence, the acknowledgements and the frames alike.
		//
		// @param datagram   The bytes as they arrived.
		// @param nowSeconds The current time.
		// @return `false` when the datagram was refused or the connection has
		//         failed. A refusal is counted, never partly applied.
		// @since v0.19
		bool Receive(std::span<const std::byte> datagram, double nowSeconds);

		// Queues a message on a reliable channel.
		//
		// The bytes are held until they are acknowledged, which is what makes
		// this reliable and is why a caller must not send more than the window
		// allows without checking.
		//
		// @param channel    Which channel, below `MAXIMUM_CHANNELS`.
		// @param message    The encoded message.
		// @param nowSeconds The current time.
		// @return `false` when the channel is out of range, the message is over
		//         `MaximumMessageBytes`, or the connection is not established.
		// @since v0.19
		bool Send(uint8_t channel, std::span<const std::byte> message, double nowSeconds);

		// Sends a message unreliably, as an RFC 9221 DATAGRAM frame.
		//
		// **Never retransmitted, and that is the point.** A late position update
		// is worse than a dropped one, because the next is already on its way
		// and is more correct than the one being waited for.
		//
		// @param channel    Which channel, below `MAXIMUM_CHANNELS`.
		// @param message    The encoded message. Refused whole when it does not
		//        fit one datagram, never fragmented.
		// @param nowSeconds The current time.
		// @return `false` when it did not go.
		// @since v0.19
		bool SendUnreliable(uint8_t channel, std::span<const std::byte> message, double nowSeconds);

		// Writes everything that is ready and drives every timer.
		//
		// **The one place this connection does I/O**, and the one place its
		// clock advances. Call it once a tick, and again whenever
		// `ExpirySeconds` has passed.
		//
		// @param nowSeconds The current time.
		// @return The number of datagrams handed to the transport.
		// @since v0.19
		size_t Flush(double nowSeconds);

		// When the next timer expires.
		//
		// A caller that only flushes on its own tick is correct and will
		// retransmit late; one that wants the protocol's own timing flushes
		// again at this time.
		//
		// @return The time, or infinity when nothing is pending.
		// @since v0.19
		double ExpirySeconds() const;

		// Begins a polite close.
		//
		// **Not a formality**, for the reason `net/AGENTS.md` gives: a peer that
		// left politely must be distinguishable from one that crashed, or every
		// clean exit costs the other end a full idle timeout.
		//
		// @param nowSeconds The current time.
		// @since v0.19
		void Close(double nowSeconds);

		// The messages that have arrived and not yet been taken.
		//
		// Reliable ones are in the order they were sent within a channel, with a
		// gap held until it is filled, and **across channels there is no order
		// and deliberately none** - that independence is what the streams are
		// for.
		//
		// @return The messages.
		// @since v0.19
		std::span<const Arrival> Inbound() const;

		// Drops the arrived messages, once the caller has applied them.
		//
		// @since v0.19
		void ClearInbound();

		// What this connection is doing.
		//
		// @return The state.
		// @since v0.19
		ConnectionState State() const;

		// Whether this end is the server.
		//
		// @return `true` for a connection made by `Accept`.
		// @since v0.19
		bool IsServer() const;

		// The peer this talks to.
		//
		// @return Its address.
		// @since v0.19
		const Endpoint &Peer() const {
			return Address;
		}

		// The connection ids this end currently answers to.
		//
		// A server routes an inbound datagram to a connection by the destination
		// connection id in its header, and QUIC lets an endpoint hand out more
		// than one so a peer can rotate them. A listener re-reads this after
		// every `Flush` and keeps its table in step.
		//
		// @return The ids, each `CONNECTION_ID_BYTES` long.
		// @since v0.19
		std::span<const std::array<std::byte, CONNECTION_ID_BYTES>> LocalIds() const;

		// The peer's Ed25519 identity, once the handshake has authenticated it.
		//
		// @return The key, or empty before the handshake completes and on a
		//         server, which asks for no client certificate.
		// @since v0.19
		std::span<const std::byte> PeerIdentity() const;

		// Why the connection ended, for a log line.
		//
		// @return A view valid for the lifetime of the process. Empty while it
		//         is healthy.
		// @since v0.19
		const char *Failure() const;

		// What this connection has done.
		//
		// @since v0.19
		struct Statistics {
			// Datagrams handed to the transport.
			uint64_t Sent = 0;

			// Datagrams the transport refused - full, closed or unreachable.
			uint64_t Undeliverable = 0;

			// Datagrams refused as not being a packet for this connection, or
			// as not opening. QUIC does not separate the two the way
			// `replication::Session` does, because a packet that does not
			// decrypt is indistinguishable from one for another connection.
			uint64_t Refused = 0;

			// Reliable messages queued.
			uint64_t Messages = 0;

			// Unreliable messages that went out as DATAGRAM frames.
			uint64_t Datagrams = 0;

			// Unreliable messages the connection would not carry - too large for
			// one datagram, or a peer that offered no datagram support.
			uint64_t DatagramsRefused = 0;

			// **Unreliable messages the peer acknowledged.** The counter
			// `D00014` says the hand-rolled stack structurally cannot have: a
			// DATAGRAM frame is never retransmitted and its containing packet
			// still is acknowledged, so a server publishing a still world sees
			// the whole outbound stream rather than the reliable slice of it.
			uint64_t DatagramsAcknowledged = 0;

			// Unreliable messages the peer never acknowledged.
			uint64_t DatagramsLost = 0;

			// The smoothed round trip, in milliseconds. Zero means unknown, not
			// instant - a connection that has sent nothing has nothing to
			// measure.
			double RoundTripMilliseconds = 0.0;

			// The congestion window ngtcp2 is pacing against, in bytes.
			uint64_t CongestionWindow = 0;

			// Packets the loss detector gave up on.
			uint64_t PacketsLost = 0;
		};

		// What this connection has done.
		//
		// @return The statistics, refreshed from ngtcp2 on every call.
		// @since v0.19
		Statistics Stats() const;

		// Everything ngtcp2-shaped, defined in the implementation and opaque
		// here. Named in public only because the callback table ngtcp2 is handed
		// has to reach it, and a friend declaration per callback would be worse
		// documentation than one line saying so.
		struct Internals;

	  private:
		Connection(Transport &transport, const Endpoint &peer, ConnectionSettings settings);

		Transport *Wire = nullptr;
		Endpoint Address;
		std::unique_ptr<Internals> Inside;
	};

	// Whether a datagram is a client's opening Initial packet.
	//
	// A server reads this before it has a connection to ask, which is what makes
	// it a free function rather than a method. It says nothing about *who* sent
	// it: deciding that belongs above this module, and `net/AGENTS.md` is
	// explicit that an unanswered stranger must cost zero bytes of state.
	//
	// @param datagram The bytes as they arrived.
	// @return `true` when this could open a connection.
	// @since v0.19
	bool Accepts(std::span<const std::byte> datagram);

	// The destination connection id a datagram is addressed to.
	//
	// How a server routes an inbound datagram to one of many connections.
	//
	// @param datagram The bytes as they arrived.
	// @param out      Where the id goes.
	// @return `false` when the datagram is not a QUIC packet, or carries an id
	//         that is not this implementation's length - which is a packet for
	//         somebody else and not an error.
	// @since v0.19
	bool RouteOf(std::span<const std::byte> datagram, std::array<std::byte, CONNECTION_ID_BYTES> &out);
}
