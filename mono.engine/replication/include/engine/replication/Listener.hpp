#pragma once

// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/QuicSession.hpp>
#include <engine/replication/Session.hpp>
#include <engine/replication/SessionPort.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// How a listener admits and streams.
	//
	// @since v0.3
	struct ListenerSettings {
		// Which transports this listener answers, and **the server decides.**
		//
		// **`Quic` is the default as of v0.19.** A client gets no say: it opens
		// with QUIC, and a listener that does not serve it answers with a
		// refusal rather than with silence, so the fallback costs one round trip
		// instead of a handshake deadline. A flag on both ends that had to agree
		// is a flag that will disagree, and what that produces is a connection
		// that hangs with nothing saying why.
		//
		// `Both` opens no second socket and starts no second accept loop. One
		// UDP port carries either, told apart by `net::WireOf` on the first
		// packet from an unknown peer.
		//
		// @since v0.19
		net::WireMode Wire = net::WireMode::Quic;

		// How a QUIC session is configured, when `Wire` serves QUIC.
		//
		// **The server's identity lives in here** - `Quic.Connection.Tls.Seed`
		// and `HasSeed` - because under QUIC the identity is the TLS one and a
		// listener with none cannot answer a handshake at all. That is the same
		// Ed25519 key `SetIdentity` takes for the datagram wire, so an operator
		// keeps one key across both: `assets::SigningKey::FromSeed` and
		// `net::quic::IdentityFor` over one seed produce the same public half.
		//
		// **A listener serving QUIC with no seed draws an ephemeral one** rather
		// than refusing to start. QUIC has no anonymous mode - the handshake
		// needs a key whether or not anybody pinned it - so the alternative to
		// drawing one is a default transport that will not serve without a
		// command-line flag. What it gives is exactly what the datagram wire's
		// anonymous mode gives: encrypted against a listener, open to a relay,
		// and said out loud in a warning.
		//
		// @since v0.19
		QuicSessionSettings Quic;

		// The link's own settings - framing, timeouts and per-tick budgets.
		SessionSettings Session;

		// How the world is streamed once a client is in.
		AuthoritySettings Authority;

		// The hard cap on connected clients.
		//
		// **In front of the handshake rather than behind it**, and that ordering
		// is what makes the stateless challenge a gap rather than a hole: the
		// worst an unadmitted flood can do is fill this number of slots, which
		// is a full server rather than an exhausted one.
		size_t MaximumClients = 64;

		// How the admission cookie is keyed and rotated.
		net::CookieSettings Cookie;
	};

	// Serves the authoritative world to every connected client.
	//
	// @since v0.3
	class Listener {
	  public:
		// Serves on a transport.
		//
		// @param transport The wire. Borrowed, not owned: one socket serves
		//                  every peer, so the caller keeps it.
		// @param settings  How to admit and stream.
		explicit Listener(net::Transport &transport, const ListenerSettings &settings = {});

		// What is replicated, and to whom.
		//
		// @return The authority.
		replication::Authority &Authority() {
			return Authority_;
		}

		// What is replicated, and to whom.
		//
		// @return The authority.
		const replication::Authority &Authority() const {
			return Authority_;
		}

		// Decides who is allowed to connect at all.
		//
		// @param policy Called as `policy(Applicant)`. An empty policy is the
		//        default above.
		void SetAdmission(AdmissionPolicy policy);

		// --- carrying somebody else's messages ---------------------------------
		//
		// **A connected, admitted, encrypted, reliable link is expensive to
		// build and this module already has one.** A caller that needs to carry
		// something else between the same two ends - a game's remote call, the
		// studio's edit stream - should widen this rather than stand up a
		// fourth session type beside it, which is the mistake this pair exists
		// to prevent.
		//
		// The payload is opaque and stays opaque. A listener that parsed one
		// would be a listener that knows what a studio edit is.

		// Sends a message to one client.
		//
		// @param client     Who to send it to.
		// @param message    The payload.
		// @param nowSeconds The current time.
		// @return `false` for an unknown client, or when the link refused it -
		//         over budget, closed, or not yet admitted. A caller that has to
		//         know whether it landed reads this.
		// @since v0.13
		bool SendTo(ClientId client, std::span<const std::byte> message, double nowSeconds);

		// Sets the added one-way delay for one admitted client's outgoing stream.
		//
		// @return `false` when the handle is unknown or stale.
		// @since v0.18
		bool SetSimulatedLatency(ClientId client, double milliseconds);

		// Sends a message to every admitted client except one.
		//
		// **The relay a shared document needs, and the exception is the point.**
		// A host that echoed an edit back to whoever sent it would have them
		// apply their own change twice.
		//
		// @param message    The payload.
		// @param nowSeconds The current time.
		// @param except      Who not to send it to. A default `ClientId` is
		//        nobody, which sends to all.
		// @return How many clients took it.
		// @since v0.13
		size_t Broadcast(std::span<const std::byte> message, double nowSeconds, ClientId except = {});

		// Hears the messages this module does not read.
		//
		// Called from `Poll`, on the thread that called it, before anything
		// else is done with the datagram.
		//
		// @param handler Called as `handler(client, payload)`.
		// @since v0.13
		void OnUserMessage(std::function<void(ClientId, std::span<const std::byte>)> handler);

		// Offers every datagram to somebody else before this listener reads it.
		//
		// **One socket, more than one protocol, and the reason is physical.** A
		// router's NAT mapping belongs to a *port*: a hole punched on some
		// other socket gets that socket through and leaves this one exactly as
		// unreachable as it was. So a server that wants to be reachable
		// peer-to-peer has to run its rendezvous traffic over the port its
		// clients will connect to - which means something has to arrive here
		// and not be a `net::Packet`.
		//
		// **Routing is by magic and is safe rather than lucky.**
		// `net::Packet::MAGIC` is `ATN1`; the discovery module's frames are
		// `ATNA` and `ATNR`. A handler that claims a datagram it should not
		// have is a handler with a bug, not an ambiguity in the formats.
		//
		// Consulted before anything else, including the source check: a
		// rendezvous message arrives from a coordination point this listener
		// has never heard of, and would otherwise be counted as a refusal.
		//
		// @param handler Called as `handler(datagram, from)`, returning whether
		//        it took the datagram. An empty handler is the default and
		//        costs one branch per datagram.
		// @since v0.13
		void SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler);

		// Gives this server an identity, so a client can tell it from a relay.
		//
		// @param key The server's signing key, borrowed for the listener lifetime.
		// @since v0.9
		void SetIdentity(const assets::SigningKey *key);

		// Decides whether a proven client identity is welcome.
		//
		// @param policy Called as `policy(ClientId, const assets::PublicKey &)`.
		// @since v0.9
		void SetClientPolicy(std::function<bool(ClientId, const assets::PublicKey &)> policy);

		// Requires every client to prove an identity before it is replicated to.
		//
		// **Withholds world state; it does not refuse admission.** A claim
		// arrives after the handshake, so there is nothing to check at the door
		// - `Publish` is where this is enforced, and an unidentified peer holds
		// a session that receives nothing until it identifies or times out.
		//
		// @param required Whether a claim is mandatory.
		// @since v0.9
		void RequireClientIdentity(bool required);

		// What a client proved, or nothing.
		//
		// @param client The client.
		// @return Its key, or nothing when it has not identified.
		std::optional<assets::PublicKey> IdentityOf(ClientId client) const;

		// Whether this listener can admit anybody at all.
		// @return `true` when the challenge secret was drawn.
		bool Admitting() const {
			return Cookie_.has_value();
		}

		// Takes everything waiting on the transport.
		// @param nowSeconds The current time.
		void Poll(double nowSeconds);

		// Builds this tick's messages for every client and sends them.
		// @param store      The authoritative world.
		// @param tick       The tick just completed.
		// @param nowSeconds The current time.
		void Publish(ecs::Store &store, uint64_t tick, double nowSeconds);

		// Advances every link and resets its per-tick budget.
		// @param nowSeconds The current time.
		void Advance(double nowSeconds);

		// Sends what is queued and resends what has gone unacknowledged.
		//
		// **What a listener that never publishes a world still has to do.**
		// Until v0.13 the only flush was inside `Publish`, which was true of
		// every caller: a server publishes every tick. A listener carrying
		// nothing but user messages does not, and without this its
		// acknowledgements never leave - so the far side's reliable window
		// fills, its payloads are resent until the resend limit, and a link
		// that is working perfectly gives up.
		//
		// `Publish` still flushes, so a server that publishes need not call
		// this and a caller that does is flushing twice into a sender that
		// sends by due time. That is a no-op rather than a double send.
		//
		// @param nowSeconds The current time.
		// @return How many peers had something to send.
		// @since v0.13
		size_t Flush(double nowSeconds);

		// The inputs every client has sent and the game has not consumed.
		//
		// @since v0.3
		struct Submission {
			// Who sent them.
			ClientId Client;

			// What they sent, in the order they sent it.
			//
			// **A span into the listener's own storage and not a copy**, so it
			// is valid until the next pump - a caller that kept it across one
			// would be reading inputs that have since been retired.
			std::span<const Input> Inputs;
		};

		// Every client's pending inputs.
		//
		// @return The submissions, valid until the next `Poll`.
		std::vector<Submission> Inputs() const;

		// Drops what a game has applied.
		void ClearInputs();

		// Applies the state every client sent for the entities it owns.
		//
		// **A host calls this once per tick, before its simulation runs.** What
		// gets through is `Authority::SetOwnership`'s business; this is only the
		// loop over connected clients, which a host cannot write for itself
		// because the peer list is private.
		//
		// @param store The world to write into.
		// @since v0.13
		void ApplyOwnedState(ecs::Store &store);

		// Hears about a client that has just been let in.
		//
		// **The hook a host needs in order to have players at all.** Who is in a
		// game is the host's business - `scene::AddPlayer` says so in as many
		// words - so this module admits a connection and says who it was rather
		// than inventing an occupant for a world it cannot see.
		//
		// Called from `Poll`, on the thread that called it, after the welcome
		// has gone out.
		//
		// @param handler Called as `handler(ClientId)`.
		// @since v0.13
		void OnAdmitted(std::function<void(ClientId)> handler);

		// Hears about a client that has gone.
		//
		// Called before the handle is retired, so a host may still use it to
		// find whatever it hung off that client.
		//
		// @param handler Called as `handler(ClientId)`.
		// @since v0.13
		void OnDropped(std::function<void(ClientId)> handler);

		// One client's link round-trip estimate, in milliseconds.
		//
		// @param client The client.
		// @return The estimate, or zero for an unknown client or an unmeasured
		//         link.
		float RoundTripMilliseconds(ClientId client) const;

		// How many clients are connected.
		//
		// **Includes a QUIC peer whose handshake has not finished**, because a
		// slot is what it occupies against `MaximumClients` from the moment its
		// first packet arrives. A caller asking "how many people are in this
		// session" wants `Carrying` instead.
		//
		// @return The count.
		size_t Count() const {
			return Peers.size();
		}

		// How many clients can be sent a message right now.
		//
		// **The number `Count` does not give, and QUIC is why it had to exist.**
		// A datagram peer is admitted the moment its answer verifies, so the two
		// were always equal; a QUIC connection exists before its handshake
		// finishes, and `SendTo` and `Broadcast` both refuse a peer that cannot
		// carry yet. A host that published to `Count` peers and read the answer
		// as delivered would lose whatever it sent in that window.
		//
		// @return The count.
		// @since v0.19
		size_t Carrying() const;

		// What this listener has done.
		//
		// @since v0.3
		struct Statistics {
			// Clients that completed the handshake and were let in.
			uint64_t Admitted = 0;

			// Clients that were in and are not any more.
			uint64_t Dropped = 0;

			// Clients turned away because the server was full.
			uint64_t Turned = 0;

			// Challenges issued to peers that had not answered one yet.
			//
			// **This is the number that stays cheap under a flood.** A challenge
			// costs no state at all, so a rising count here beside a flat
			// `Admitted` is somebody knocking rather than a server in trouble.
			uint64_t Challenged = 0;

			// Datagrams refused before they became anything - a wrong magic, an
			// unknown version, a channel outside the enum.
			uint64_t Refused = 0;

			// Peers turned away for opening with the stack this listener does
			// not serve.
			//
			// **Apart from `Refused` on purpose.** A refusal is a datagram that
			// was not anything; this is one that was a perfectly good opening
			// message for the other transport. A count rising here is a
			// deployment whose clients and whose `--transport` disagree, which
			// is a different fix from a port somebody is probing.
			//
			// @since v0.19
			uint64_t Mismatched = 0;

			// Peers that completed the handshake and were declined by the
			// admission policy.
			//
			// **Apart from `Turned` on purpose**: full is the engine's answer
			// and rejected is the game's, and a server that is turning people
			// away wants a different fix from one whose policy is declining
			// them.
			uint64_t Rejected = 0;
		};

		// What this listener has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		struct Peer {
			net::Endpoint Where;
			ClientId Client;

			// Whichever session this listener's wire produces. See
			// `SessionPort.hpp` for why there are two of them and one interface.
			std::unique_ptr<SessionPort> Wire;

			// The same object as `Wire` when the wire is QUIC, and null
			// otherwise. What it is for is the two questions a session cannot
			// answer: which connection ids this end responds to, so a datagram
			// can be routed, and who the peer proved itself to be.
			QuicSession *Quic = nullptr;

			// Whether the admitted handler has been told about this peer.
			//
			// **Deferred under QUIC, and it has to be.** A datagram peer is
			// admitted the moment its answer verifies; a QUIC peer is a
			// connection that exists before its handshake finishes, and telling
			// a host it has a player before the peer has proved anything would
			// be a player that vanishes when the handshake fails.
			bool Announced = false;

			std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

			// Keep the original frame for safe retransmission. Datagram wire
			// only: QUIC repeats its own handshake flight.
			std::vector<std::byte> Welcome;

			std::optional<assets::PublicKey> Identity;
		};

		// What an identity claim is signed over. The admission transcript on the
		// datagram wire and a TLS exporter under QUIC - `SessionPort::Binding`
		// is the seam, and the length is the same either way so that one
		// signature format serves both.
		using Binding = std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES>;

		Peer *Find(const net::Endpoint &from);
		bool Route(std::span<const std::byte> datagram, double nowSeconds);
		void Refuse(const net::Endpoint &from, net::WireKind opening, std::span<const std::byte> datagram);
		void Arrive(const net::Endpoint &from, std::span<const std::byte> datagram, double nowSeconds);
		void Deliver(Peer &peer);
		void Announce(Peer &peer);
		void Greet(const net::Endpoint &from, std::span<const std::byte> datagram, double nowSeconds);
		void Challenge(const net::Endpoint &from, const replication::Hello &hello, double nowSeconds);
		void Accept(const net::Endpoint &from, const replication::Answer &answer, double nowSeconds);
		void Repeat(Peer &peer, const Admission &message);
		void Drop(size_t index);

		net::Transport *Transport_;
		ListenerSettings Settings;

		const assets::SigningKey *Identity = nullptr;

		bool RequireIdentity = false;
		std::function<bool(ClientId, const assets::PublicKey &)> ClientPolicy;
		replication::Authority Authority_;

		// Missing entropy fails closed; no peer is admitted.
		std::optional<net::Cookie> Cookie_;

		AdmissionPolicy Policy;

		// Whoever else is sharing this socket. See SetForeign.
		std::function<bool(std::span<const std::byte>, const net::Endpoint &)> Foreign;

		// Whoever is carrying something over these links. See OnUserMessage.
		std::function<void(ClientId, std::span<const std::byte>)> UserMessages;

		std::vector<Peer> Peers;

		std::function<void(ClientId)> Admitted_;
		std::function<void(ClientId)> Dropped_;

		std::vector<std::byte> Datagram;

		std::vector<std::byte> Reply;

		uint32_t NextConnection = 1;

		Statistics Stats_;
	};
}
