#pragma once

// The authoritative end, over a transport, for however many clients turn up.
//
// `Session` is one peer and `Authority` is what to tell them; this is the thing
// that holds many of the first and one of the second and runs them from a tick.
// **It exists so that the loop is written once.** A server and a client each
// need "drain the transport, route each datagram to the session that owns it,
// hand what came out to the replication half, send what it produced, flush the
// resends" — and the two copies of that which do not share code are the two that
// eventually disagree about whether the flush happens before or after the
// acknowledgement. `world::Driver` makes the same argument about there being one
// router.
//
// **A client is admitted on its first datagram, and that is not
// authentication.** Anybody who can reach the port can occupy a slot. `net`'s
// `Handshake` exists — X25519 into HKDF into ChaCha20-Poly1305 — and is
// deliberately not wired in here: doing it properly means a key exchange, a
// challenge the client has to answer before a slot is reserved, and a policy for
// who is allowed at all, and none of those are the thing this class is for. What
// *is* here is the bound: `MaximumClients` slots, and datagrams from a stranger
// past that are refused and counted rather than allocated for. That turns an
// unbounded memory attack into a full server, which is the difference between a
// gap and a hole. See `docs/DEFERRED.md`.
//
// **Time is passed in, never read** — `replication/AGENTS.md`, and the same
// rule the two layers under this follow.
//
// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Session.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::replication {

	// How a listener admits and streams.
	//
	// @since v0.3
	struct ListenerSettings {
		// How each peer's session frames and resends.
		SessionSettings Session;

		// How the authority chunks and streams.
		AuthoritySettings Authority;

		// The most clients that may be admitted at once.
		//
		// The bound that makes an unauthenticated admit a full server rather
		// than an out-of-memory kill. Sized for a game rather than for a stress
		// test: a slot costs a session, a link, two reliability windows and a
		// per-client known set, so this is not a number to raise casually.
		size_t MaximumClients = 64;
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
		// Exposed rather than wrapped: `Replicate` and `SetInterest` are
		// decisions a game makes and this class has nothing to add to them.
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

		// Takes everything waiting on the transport.
		//
		// Admits a client for an endpoint not seen before, up to
		// `MaximumClients`. Call before `Publish`, so an acknowledgement that
		// arrived this tick is counted before this tick's delta is built.
		//
		// @param nowSeconds The current time.
		void Poll(double nowSeconds);

		// Builds this tick's messages for every client and sends them.
		//
		// Call after the world has ticked and **before its change bits are
		// cleared** — the bits are the delta source, and clearing them first is
		// how a tick's worth of movement goes missing.
		//
		// @param store      The authoritative world.
		// @param tick       The tick just completed.
		// @param nowSeconds The current time.
		void Publish(ecs::Store &store, uint64_t tick, double nowSeconds);

		// Advances every link and resets its per-tick budget.
		//
		// Separate from `Publish` because a server with nothing to say still has
		// to notice a peer that stopped answering.
		//
		// @param nowSeconds The current time.
		void Advance(double nowSeconds);

		// The inputs every client has sent and the game has not consumed.
		//
		// One entry per client that sent anything, so a game applies them and
		// then calls `ClearInputs`.
		//
		// @since v0.3
		struct Submission {
			// Who sent them.
			ClientId Client;

			// What they sent, oldest first.
			std::span<const Input> Inputs;
		};

		// Every client's pending inputs.
		//
		// @return The submissions, valid until the next `Poll`.
		std::vector<Submission> Inputs() const;

		// Drops what a game has applied.
		void ClearInputs();

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
			// Clients admitted since the process started.
			uint64_t Admitted = 0;

			// Clients dropped because their link ended.
			uint64_t Dropped = 0;

			// Datagrams from a stranger refused because every slot was taken.
			uint64_t Turned = 0;
		};

		// What this listener has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		// One connected client: who they are to the authority, and the session
		// carrying their bytes.
		struct Peer {
			net::Endpoint Where;
			ClientId Client;
			std::unique_ptr<Session> Wire;
		};

		Peer *Find(const net::Endpoint &from);
		Peer *Admit(const net::Endpoint &from, double nowSeconds);
		void Drop(size_t index);

		net::Transport *Transport_;
		ListenerSettings Settings;
		replication::Authority Authority_;

		std::vector<Peer> Peers;

		// Reused across ticks so a server polling every frame stops allocating.
		std::vector<std::byte> Datagram;

		// The next connection index to hand a session, and the generation each
		// slot is on. A connection id is `net`'s, not this module's, and it has
		// the same reuse rule every handle here does.
		uint32_t NextConnection = 1;

		Statistics Stats_;
	};
}
