#pragma once

// Two peers behind two routers, and the third party that introduces them.
//
// A LAN announcement works because both machines are on one link. Across the
// internet neither one can be addressed until it has sent something outward:
// a home router keeps no mapping for a port nobody has used, so the first
// datagram in either direction is dropped by the router of whoever did not move
// first. **Both sides have to move first, and something that can already reach
// both of them has to say when.** That is the whole of what a rendezvous point
// is.
//
// The sequence, and there is nothing else in it:
//
// 1. A host **registers** with the point. The point sees the address the
//    registration arrived from — the *reflexive* address, the one the host's
//    router put on it — and tells the host what that is.
// 2. A guest asks the point to **reach** that session by id.
// 3. The point sends a **punch** to each side naming the other's reflexive
//    address, and a nonce it drew for this meeting.
// 4. Both sides **poke** the address they were given, repeatedly. The first
//    poke out of each router creates that router's mapping; the first poke that
//    *arrives* is the one that found a mapping already open. One of them is
//    dropped and the other gets through, and which is which does not matter.
// 5. The side that received a poke answers it. Both sides now hold an address
//    that works, and this module is finished — what happens over that address
//    is `replication`'s, or `delivery`'s, or nobody's.
//
// ## What this deliberately does not do
//
// **No relay.** When both routers refuse — symmetric NAT on both ends, which is
// a minority of a minority — the attempt fails and says so, rather than falling
// back to carrying the session's traffic through the point. A relay is a
// different product: it is bandwidth somebody pays for, it is a bottleneck with
// a latency floor, and it is the piece that turns a small coordination service
// into an operational commitment. `ReachState::Failed` is an honest answer and
// a hidden relay is not.
//
// **No matchmaking, no ranking, no accounts.** The point holds an id, an
// address, a copy of the advert, and when it last heard from it. Everything it
// would need to rank sessions is everything it would need to be a service with
// users, and this is a piece of plumbing that a content origin runs on the
// side.
//
// ## A private session is unlisted, not gated
//
// **The point cannot check a key it does not hold, and it must not hold one.**
// Giving it the key would make every operator of a rendezvous point a holder of
// every private session's secret, which is precisely the trust this module
// exists to avoid needing. So a `Private` registration is simply absent from
// every browse reply: reaching one requires its `SessionId`, which is 128 bits
// the host handed over along with the key.
//
// What the key *does* do here is the poke. Both sides tag `SessionId || Nonce`
// under it, and a private host drops a poke whose tag does not verify. That is
// possession of the key plus return routability, and it is not a session
// establishment — the connection made over the resulting address authenticates
// on its own terms, one layer up.
//
// @tier shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <network/Advert.hpp>
#include <network/Directory.hpp>
#include <network/SessionKey.hpp>
#include <optional>
#include <span>
#include <vector>

namespace network {

	// The default port a rendezvous point listens on.
	//
	// One past `DISCOVERY_PORT`, so an operator reading a firewall rule can see
	// the pair belongs together. Picked rather than derived, like the other.
	constexpr uint16_t RENDEZVOUS_PORT = 47601;

	// How many bytes of nonce a meeting is stamped with.
	constexpr size_t MEETING_NONCE_BYTES = 8;

	// Where an attempt to reach a peer has got to.
	//
	// Forward only within one attempt, like every other lifecycle in this
	// repository: a `Failed` attempt stays failed and a second try is a second
	// call to `Reach`. An attempt that could restart itself is one every caller
	// has to re-check after every pump.
	//
	// @since v0.13
	enum class ReachState : uint8_t {
		// Nothing has been asked for.
		Idle = 0,

		// The point has been asked and has not answered.
		Locating = 1,

		// A punch arrived and pokes are going out. The peer's address is known
		// and has not answered yet.
		Punching = 2,

		// A poke was exchanged. `Reached` is an address that works.
		Reached = 3,

		// The point did not know the session, or nothing answered before
		// `GiveUpAfterSeconds`.
		Failed = 4,
	};

	// Returns a stable, human-readable name for a reach state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ReachState state);

	// Whether a datagram is one of this module's rendezvous messages.
	//
	// **This exists so that a punch can happen on the socket that will carry
	// the session, which is the only socket a punch is worth anything on.** A
	// router's mapping belongs to a port: a hole punched on a discovery socket
	// gets a discovery socket through, and the game's socket is still as
	// unreachable as it was. So a program that wants peer-to-peer for its
	// *traffic* hands `RendezvousClient` the transport its traffic uses, drains
	// that transport itself, and offers each datagram here before treating it
	// as its own.
	//
	// Routing by magic is safe rather than lucky: `engine::net::Packet::MAGIC`
	// is `ATN1`, an advert is `ATNA` and a rendezvous message is `ATNR`. Three
	// formats, three first words, and a reader that checks its own before doing
	// anything.
	//
	// @param datagram The bytes.
	// @return Whether `Deliver` would understand them.
	// @since v0.13
	bool IsRendezvousMessage(std::span<const std::byte> datagram);

	// How a client paces its registrations and its punching.
	//
	// @since v0.13
	struct RendezvousSettings {
		// How often to say the same thing again while something is unfinished —
		// a registration unanswered, a punch unacknowledged.
		//
		// The poke interval as well as the retry interval, and the two are one
		// number on purpose: a punch that pokes faster than it retries would
		// spend its whole budget on the case where the point never answered.
		double RepeatEverySeconds = 0.25;

		// How long an attempt to reach a peer may take before it fails.
		//
		// Generous, because the failure it is measuring is a router that will
		// never cooperate, and the cost of waiting a little longer is a person
		// looking at a spinner rather than a person told the wrong thing.
		double GiveUpAfterSeconds = 8.0;

		// How often a host repeats its registration.
		//
		// Also what keeps the router's mapping alive: a NAT drops an idle UDP
		// mapping after somewhere between thirty seconds and a few minutes,
		// and a host whose mapping has expired is a host the point holds a dead
		// address for. Shorter than `PointSettings::ForgetAfterSeconds` by a
		// wide margin, for `Beacon`'s reason.
		double RegisterEverySeconds = 10.0;
	};

	// What a client has done.
	//
	// @since v0.13
	struct RendezvousCounters {
		// Registrations sent.
		uint64_t Registrations = 0;

		// Registrations the point acknowledged.
		uint64_t Acknowledged = 0;

		// Browse requests sent.
		uint64_t Browses = 0;

		// Sessions taken out of browse replies.
		uint64_t Listed = 0;

		// Reach requests sent.
		uint64_t Reaches = 0;

		// Punches received from the point.
		uint64_t Punches = 0;

		// Pokes sent to a peer.
		uint64_t Poked = 0;

		// Pokes received from a peer.
		uint64_t Answered = 0;

		// Sessions the point did not know.
		uint64_t Unknown = 0;

		// Datagrams from the point or a peer that were not a message.
		uint64_t Malformed = 0;

		// Pokes dropped for a tag that did not verify.
		uint64_t Refused = 0;

		// Sends the transport would not take.
		uint64_t Undelivered = 0;
	};

	// A process's side of a rendezvous: registering, browsing and punching.
	//
	// One object does all three because one socket has to. A punch only works
	// from the port the registration came from — that is the mapping the
	// router opened — so a client that browsed on one socket and punched on
	// another would punch a hole nobody was told about.
	//
	// Borrows its transport, like everything else here.
	//
	// @since v0.13
	class RendezvousClient {
	  public:
		// @param transport The wire. Borrowed, not owned, and the same one for
		//        the whole life of the registration.
		// @param point     Where the rendezvous point is.
		// @param settings  How often to repeat, and when to give up.
		RendezvousClient(
			engine::net::Transport &transport,
			const engine::net::Endpoint &point,
			const RendezvousSettings &settings = {}
		);

		// Starts registering this session, and keeps doing it.
		//
		// @param advert What to register. A `Private` one is registered and not
		//        listed — see the file header.
		// @param key    The session key, for a private session's pokes. Moved
		//        in.
		void Register(const Advert &advert, std::optional<SessionKey> key = std::nullopt);

		// Replaces what is registered, keeping the schedule and the key.
		//
		// @param advert The new record.
		void SetAdvert(const Advert &advert);

		// Tells the point this session is going away, and stops registering.
		//
		// **Best effort and unacknowledged, deliberately.** A process shutting
		// down has nothing to wait for, and the point's expiry is what makes a
		// host that crashed indistinguishable from one that was killed. This
		// only makes the common case tidy.
		//
		// @param nowSeconds The current time.
		void Withdraw(double nowSeconds);

		// Asks the point for the sessions it holds.
		//
		// The reply lands in the directory passed to `Pump`, as `Reach::Peer`
		// rows. Asked for rather than streamed: a point that pushed its list
		// would push it to everybody who ever registered.
		//
		// @param use        Which sessions to ask for.
		// @param nowSeconds The current time.
		void Browse(Purpose use, double nowSeconds);

		// Starts an attempt to reach one session.
		//
		// Replaces any attempt in flight — one socket punches one hole at a
		// time, and two attempts sharing it would each answer the other's
		// pokes.
		//
		// @param session    Which session, by id.
		// @param key        Its key, when it is private. Moved in.
		// @param nowSeconds The current time.
		// @return `false` for the null id.
		bool Reach(const SessionId &session, std::optional<SessionKey> key, double nowSeconds);

		// Drains the transport, answers what arrived, and repeats what is due.
		//
		// @param directory  Where browse replies are listed, or null to discard
		//        them. A host that never browses passes null and holds no
		//        table.
		// @param nowSeconds The current time.
		void Pump(Directory *directory, double nowSeconds);

		// Answers one datagram the caller already took off the wire, and
		// repeats what is due.
		//
		// For a client sharing its transport with something else — see
		// `IsRendezvousMessage`. A caller using this must still call it every
		// tick with an empty datagram, because the repeats are what a punch is
		// made of.
		//
		// @param datagram   The bytes, or empty for "nothing arrived".
		// @param from       Where they came from. Ignored when empty.
		// @param directory  Where browse replies are listed, or null.
		// @param nowSeconds The current time.
		// @return Whether the datagram was one of this module's.
		bool Deliver(
			std::span<const std::byte> datagram,
			const engine::net::Endpoint &from,
			Directory *directory,
			double nowSeconds
		);

		// Where the current attempt has got to.
		//
		// @return The state.
		ReachState State() const {
			return Phase;
		}

		// The address the peer answered on.
		//
		// @return The endpoint, or an invalid one unless the state is
		//         `Reached`.
		engine::net::Endpoint Reached() const;

		// This process's address as the point saw it.
		//
		// **Worth having for more than diagnostics**: a host whose reflexive
		// address equals its local one is a host with no NAT in front of it,
		// which is the case where a guest can simply dial it and skip the
		// punch entirely.
		//
		// @return The endpoint, or an invalid one until the point has answered.
		engine::net::Endpoint Reflexive() const {
			return Public;
		}

		// Whether the point has acknowledged this session.
		//
		// @return `true` once a registration was answered.
		bool Enrolled() const {
			return Acknowledged;
		}

		// What this client has done.
		//
		// @return The counters.
		const RendezvousCounters &Counters() const {
			return Tally;
		}

	  private:
		// A punch this process is on the receiving end of: somebody asked the
		// point to reach the session we host, and both sides now have to poke.
		//
		// Several at once, because a host being joined by three people at the
		// same moment is ordinary. Bounded, because the point can be asked to
		// introduce this host as often as anybody likes.
		struct Introduction {
			engine::net::Endpoint Peer;
			std::array<std::byte, MEETING_NONCE_BYTES> Nonce{};
			double ExpiresAtSeconds = 0.0;
			double RepeatDueAt = 0.0;
		};

		// The most punches a host answers at once.
		static constexpr size_t MAXIMUM_INTRODUCTIONS = 16;

		void SendRegistration(double nowSeconds);
		void SendPoke(
			const engine::net::Endpoint &to,
			const SessionId &session,
			const std::array<std::byte, MEETING_NONCE_BYTES> &nonce,
			const SessionKey *key,
			bool answering
		);
		void Handle(
			std::span<const std::byte> datagram,
			const engine::net::Endpoint &from,
			Directory *directory,
			double nowSeconds
		);
		void Repeat(double nowSeconds);

		engine::net::Transport &Wire;
		engine::net::Endpoint Point;
		RendezvousSettings Limits;

		// The registration, and the key its pokes are tagged under.
		std::optional<Advert> Registering;
		std::optional<SessionKey> HostKey;
		double RegisterDueAt = 0.0;
		bool Registrable = false;
		bool Acknowledged = false;
		engine::net::Endpoint Public;

		// The attempt in flight.
		ReachState Phase = ReachState::Idle;
		SessionId Target;
		std::optional<SessionKey> ReachKey;
		engine::net::Endpoint Peer;
		std::array<std::byte, MEETING_NONCE_BYTES> Nonce{};
		double AttemptStartedAt = 0.0;
		double RepeatDueAt = 0.0;

		// The punches this process is answering as a host.
		std::vector<Introduction> Introductions;

		RendezvousCounters Tally;
		std::vector<std::byte> Scratch;
	};

	// How much a point holds, and for how long.
	//
	// @since v0.13
	struct PointSettings {
		// How long since the last registration before a session is dropped.
		double ForgetAfterSeconds = 30.0;

		// The most sessions to hold.
		//
		// The same bound `Directory` has and for the same reason: an open port
		// receives whatever is sent to it, and a table that grew with what
		// arrived is a table a stranger fills.
		size_t MaximumSessions = 256;

		// The most sessions one browse reply may carry.
		//
		// Bounded by the datagram, not by taste: a reply has to fit in one
		// `engine::net::Transport::MAXIMUM_DATAGRAM_BYTES`, and an advert with
		// two full-length strings is not small. A browser that wants more than
		// this asks again — which nothing implements yet, and saying so is
		// better than implying a paging protocol exists.
		uint8_t ListingsPerReply = 8;
	};

	// What a point has served.
	//
	// @since v0.13
	struct PointCounters {
		// Sessions registered for the first time.
		uint64_t Registrations = 0;

		// Registrations that refreshed a session already held.
		uint64_t Refreshes = 0;

		// Sessions withdrawn politely.
		uint64_t Withdrawals = 0;

		// Browse requests answered.
		uint64_t Browses = 0;

		// Introductions made.
		uint64_t Introductions = 0;

		// Reach requests for a session this point does not hold.
		uint64_t Unknown = 0;

		// Datagrams that were not a message.
		uint64_t Malformed = 0;

		// Registrations refused because the table was full.
		uint64_t Full = 0;

		// Sessions dropped for going quiet.
		uint64_t Forgotten = 0;

		// Sends the transport would not take.
		uint64_t Undelivered = 0;
	};

	// The third party that introduces two peers, and holds nothing else.
	//
	// Run by whoever is already running something reachable — `mono.cdn` hosts
	// one with `--rendezvous-listen`. It is not a program of its own, because a
	// second executable doing this would be a second deployment to configure
	// and a second place for the message set to drift.
	//
	// @since v0.13
	class RendezvousPoint {
	  public:
		// @param settings How much to hold, and for how long.
		explicit RendezvousPoint(const PointSettings &settings = {});

		// Drains a transport and answers what arrived.
		//
		// @param transport  The socket the point listens on.
		// @param nowSeconds The current time.
		// @return How many datagrams were understood.
		size_t Serve(engine::net::Transport &transport, double nowSeconds);

		// Drops every session that has gone quiet.
		//
		// @param nowSeconds The current time.
		// @return How many were dropped.
		size_t Forget(double nowSeconds);

		// How many sessions are held.
		//
		// @return The count.
		size_t Holding() const {
			return Sessions.size();
		}

		// What this point has served.
		//
		// @return The counters.
		const PointCounters &Counters() const {
			return Tally;
		}

	  private:
		// One registered session. The advert is kept as the bytes it arrived
		// as, not as a decoded record: a browse reply repeats them unchanged,
		// so a point neither re-encodes an advert nor invalidates the tag on
		// one it cannot verify.
		struct Enrolment {
			SessionId Session;
			Purpose Use = Purpose::Game;
			Access Admits = Access::Public;
			engine::net::Endpoint From;
			std::vector<std::byte> AdvertBytes;
			double LastSeenSeconds = 0.0;
		};

		Enrolment *Find(const SessionId &session);

		PointSettings Limits;
		std::vector<Enrolment> Sessions;
		PointCounters Tally;
		std::vector<std::byte> Scratch;
	};
}
