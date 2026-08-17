#pragma once

// The host's half of relay mode: a client asks for a content route and this
// answers it, at a rate this end decides.
//
// **The client has no authority here and that is the whole design.** It may ask
// for one of the three routes an origin serves and it may ask again, and every
// bound on how often, how many at once and how fast is held on this side -
// because a client is untrusted and a limiter that lived in the client would be
// a limiter the interesting clients do not run. `replication::Disputed` states
// the same rule for the same reason: *the answer is upstream traffic from a
// peer, so the limit is the server's.*
//
// **Content never outranks the simulation.** A relayed chunk goes out through
// the same per-tick link budget everything else spends, and it is offered
// *after* the world has been published - so a link that is full of world is a
// link that carries no content this tick and carries it on the next.
// `Listener::SendTo` refusing is ordinary backpressure, exactly as
// `Link::Reserve` refusing is for a delta, and the piece is offered again
// rather than lost.
//
// **What it is not.** It does not verify content, decide who is entitled to
// what, or parse a manifest. A client verifies end to end against a signed root
// whatever route the bytes took; a grant is `assets::Grant`'s and
// `cdn::Gate`'s; and the bytes come out of `Engine::delivery`, which is the one
// implementation of a route.

#include <engine/delivery/Relay.hpp>
#include <engine/replication/Authority.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace server {

	// What one client may ask of a relay.
	//
	// @since v0.16
	struct ContentRelayLimits {
		// Requests a client may make per second, sustained.
		double RequestsPerSecond = 8.0;

		// How many it may make in one burst.
		//
		// **A bucket rather than a hard gap between requests**, because a client
		// that has just loaded a world legitimately asks for a manifest, a
		// dictionary and a handful of bundles at once, and a limiter that made
		// that pathological would be a limiter every deployment turns off.
		double Burst = 16.0;

		// How many routes one client may have outstanding.
		//
		// Each one is a group held in this process's memory on that client's
		// behalf, which is the resource a flood is really after.
		size_t OutstandingPerClient = 2;

		// How many refused-for-rate requests before a client is flagged.
		//
		// @see ContentRelayStatistics::Flagged
		uint64_t FloodThreshold = 32;

		// How long a flagged client is refused outright.
		//
		// **Refused rather than disconnected.** Dropping the connection would
		// make a client on a bad script indistinguishable from a client on a bad
		// network, and this engine has one thing to say about that already:
		// `Statistics::Turned` is a full server and `Rejected` is a declined
		// one, and neither is a link being torn down. A cooldown is a bound the
		// operator can read and the client can recover from.
		double FloodCooldownSeconds = 30.0;

		// How many pieces of one route go to one client per pump.
		//
		// The link's own budget is the real limit and this sits under it, so a
		// host serving one client does not spend its whole uplink on content the
		// moment somebody joins.
		size_t ChunksPerClientPerPump = 8;
	};

	// What a relay has done, for an operator and for a test.
	//
	// **Five counters and they mean five different things.** A client asking too
	// fast, a route nothing could produce and an upstream being down are three
	// incidents with three different fixes, and one number for them would bury
	// the one that matters - `assets::Grant`'s rule about forged, expired and
	// out-of-scope.
	//
	// @since v0.16
	struct ContentRelayStatistics {
		// Route requests that were parsed and accepted.
		uint64_t Requests = 0;

		// Routes answered in full.
		uint64_t Served = 0;

		// Routes no source produced, or that named something a relay does not
		// carry.
		uint64_t Refused = 0;

		// Requests dropped because the client was asking too fast.
		uint64_t Dropped = 0;

		// Clients that crossed `ContentRelayLimits::FloodThreshold`.
		uint64_t Flagged = 0;

		// Chunks the link would not take, offered again on a later pump.
		uint64_t Deferred = 0;

		// Content bytes handed to the link.
		uint64_t SentBytes = 0;
	};

	// Answers clients' route requests out of this host's own content sources.
	//
	// **One owner, one thread**, like every other pump in the delivery path.
	//
	// @since v0.16
	class ContentRelay {
	  public:
		// @param fetcher Where routes come from. Owned; never null.
		// @param limits  What one client may ask.
		explicit ContentRelay(
			std::unique_ptr<engine::delivery::RouteFetcher> fetcher, const ContentRelayLimits &limits = {}
		);

		~ContentRelay();

		ContentRelay(const ContentRelay &) = delete;
		ContentRelay &operator=(const ContentRelay &) = delete;

		// Hands the grant this host presents to its own upstream origins.
		//
		// @param token The bytes, or empty for none.
		void UseGrant(std::span<const std::byte> token);

		// Takes one message a client sent.
		//
		// **Every field of it is hostile.** A payload that is not a route
		// request at all is a non-event - the user channel is shared - and one
		// that is gets the rate check, the outstanding bound and the closed list
		// of routes before anything is fetched.
		//
		// @param client     Who sent it.
		// @param message    The payload, exactly as it arrived.
		// @param nowSeconds The current time, passed in for `net`'s rule.
		// @return Whether this was a content message at all, which is how a
		//         caller knows not to offer it to anybody else.
		bool
		Receive(engine::replication::ClientId client, std::span<const std::byte> message, double nowSeconds);

		// Drives the fetcher and sends what the link will take.
		//
		// **Called after the world has been published**, so the simulation has
		// already claimed its share of the tick's bytes.
		//
		// **No clock, unlike `Receive`.** Nothing here expires or refills; the
		// allowance is spent where a request arrives, and a parameter declared
		// ahead of a caller that needs it is indistinguishable from a mistake.
		//
		// @param send Called as `send(client, payload)`, returning whether the
		//        link took it. A refusal ends this client's turn for this pump
		//        and the piece is offered again on the next.
		void Pump(const std::function<bool(engine::replication::ClientId, std::span<const std::byte>)> &send);

		// Forgets a client that has gone.
		//
		// @param client The client.
		void Forget(engine::replication::ClientId client);

		// How many clients have something outstanding.
		size_t Busy() const;

		// What this relay has done.
		const ContentRelayStatistics &Stats() const {
			return Tally;
		}

	  private:
		// One route being answered for one client.
		struct Job {
			// What the client called it.
			uint64_t Ticket = 0;

			// What the fetcher called it, until it was taken.
			uint64_t Fetch = 0;

			// The route's bytes once they arrived.
			std::vector<std::byte> Bytes;

			// How much of them has been handed to the link.
			uint32_t Offset = 0;

			bool Ready = false;
			bool Refused = false;

			// Whether anything at all has gone out, so a zero-length route still
			// closes rather than waiting for a piece that would never be sent.
			bool Opened = false;
		};

		// One client's allowance and what it is owed.
		struct Session {
			// Whether this slot has been set up at all, which a generation cannot
			// say: a fresh map entry and the first client to occupy slot zero both
			// read generation zero.
			bool Opened = false;

			uint32_t Generation = 0;
			double Tokens = 0.0;
			double LastSeconds = 0.0;
			uint64_t Dropped = 0;
			double FlaggedUntil = 0.0;
			std::vector<Job> Jobs;
		};

		// The session for a client, resetting a slot a new client took over.
		Session &SessionFor(engine::replication::ClientId client, double nowSeconds);

		// Refills a bucket and takes one token, or refuses.
		bool Admits(Session &session, engine::replication::ClientId client, double nowSeconds);

		std::unique_ptr<engine::delivery::RouteFetcher> Routes;
		ContentRelayLimits Limits;
		// **Ordered by client index rather than hashed**, so a pump serves the
		// same clients in the same order on every run and on every machine. The
		// order decides who gets the tail of a tick's byte budget when there is
		// not enough for everybody, and a hash order would make that a property
		// of the allocator.
		std::map<uint32_t, Session> Sessions;
		ContentRelayStatistics Tally;
	};
}
