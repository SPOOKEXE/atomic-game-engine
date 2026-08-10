#pragma once

// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Session.hpp>

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
		// The link's own settings — framing, timeouts and per-tick budgets.
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

		// Offers every datagram to somebody else before this listener reads it.
		//
		// **One socket, more than one protocol, and the reason is physical.** A
		// router's NAT mapping belongs to a *port*: a hole punched on some
		// other socket gets that socket through and leaves this one exactly as
		// unreachable as it was. So a server that wants to be reachable
		// peer-to-peer has to run its rendezvous traffic over the port its
		// clients will connect to — which means something has to arrive here
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
		// — `Publish` is where this is enforced, and an unidentified peer holds
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

		// The inputs every client has sent and the game has not consumed.
		//
		// @since v0.3
		struct Submission {
			// Who sent them.
			ClientId Client;

			// What they sent, in the order they sent it.
			//
			// **A span into the listener's own storage and not a copy**, so it
			// is valid until the next pump — a caller that kept it across one
			// would be reading inputs that have since been retired.
			std::span<const Input> Inputs;
		};

		// Every client's pending inputs.
		//
		// @return The submissions, valid until the next `Poll`.
		std::vector<Submission> Inputs() const;

		// Drops what a game has applied.
		void ClearInputs();

		// One client's link round-trip estimate, in milliseconds.
		//
		// @param client The client.
		// @return The estimate, or zero for an unknown client or an unmeasured
		//         link.
		float RoundTripMilliseconds(ClientId client) const;

		// How many clients are connected.
		//
		// @return The count.
		size_t Count() const {
			return Peers.size();
		}

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

			// Datagrams refused before they became anything — a wrong magic, an
			// unknown version, a channel outside the enum.
			uint64_t Refused = 0;

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
			std::unique_ptr<Session> Wire;

			std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

			// Keep the original frame for safe retransmission.
			std::vector<std::byte> Welcome;

			// Retained for later identity verification.
			std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> Transcript{};

			std::optional<assets::PublicKey> Identity;
		};

		Peer *Find(const net::Endpoint &from);
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

		std::vector<Peer> Peers;

		std::vector<std::byte> Datagram;

		std::vector<std::byte> Reply;

		uint32_t NextConnection = 1;

		Statistics Stats_;
	};
}
