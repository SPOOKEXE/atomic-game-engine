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
// **A client is admitted after a key exchange it had to answer a challenge to
// begin, and never on its first datagram.** `Admission.hpp` has the sequence;
// what matters here is the order of the checks, because that order is the
// protection:
//
// 1. A datagram from an address with no session, on any channel but
//    `net::ChannelKind::Handshake`, is dropped. It used to be an admission.
// 2. A `Hello` is answered with a cookie `net::Cookie` *derives* rather than
//    stores. **An unanswered challenge costs zero bytes** — no slot, no
//    session, no link, no reliability window, no map entry — and it costs zero
//    however many are outstanding.
// 3. An `Answer` is checked against the cookie, then against `MaximumClients`,
//    then against the game's `SetAdmission` policy, then against the X25519
//    agreement. Cheapest first, and every one of them is a reason to stop
//    before anything is allocated.
// 4. Only then is a slot taken.
//
// **The bound stays in front of the handshake, not behind it.**
// `MaximumClients` is 64 and a peer past it is refused and counted in
// `Statistics::Turned`, exactly as before. A handshake is defence in front of
// that bound rather than a replacement for it: a slot still costs a session, a
// link, two reliability windows and a per-client known set, so the number is
// still not one to raise casually.
//
// **What the handshake proves, and what it does not.** The cookie proves the
// peer can receive at the address it wrote. The agreement proves it can do
// X25519 and gives forward secrecy for the keys it derives. Neither says who it
// is: `net::Handshake`'s own header is explicit that an unauthenticated
// agreement is safe against a listener and not against a relay, and the default
// admission policy lets in anybody who completes it. **The stream is also still
// in the clear** — the derived keys confirm the exchange and are then destroyed,
// because encrypting the traffic is `net`'s outstanding item and a `Sealer` kept
// here that nothing seals with would read as though it were not.
//
// **Time is passed in, never read** — `replication/AGENTS.md`, and the same
// rule the two layers under this follow.
//
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
#include <memory>
#include <optional>
#include <span>
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
		// The bound that makes a flood of admissions a full server rather than
		// an out-of-memory kill, and it sits in front of the key exchange
		// rather than behind it. Sized for a game rather than for a stress
		// test: a slot costs a session, a link, two reliability windows and a
		// per-client known set, so this is not a number to raise casually.
		size_t MaximumClients = 64;

		// How long a challenge stays answerable.
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

		// Decides who is allowed to connect at all.
		//
		// Asked once per peer, after its cookie has verified and before
		// anything is allocated for it, with the address the cookie proved it
		// can receive at. Answering `false` costs the peer nothing but a
		// counted refusal — no slot is taken and nothing is sent back, because
		// telling a stranger *why* it was refused is telling it what to change.
		//
		// **The default admits anybody who completes the handshake, and that is
		// stated rather than implied.** With no policy set, a peer that can
		// receive a datagram at the address it wrote and can do X25519 is let
		// in. That is a much weaker property than it sounds: the exchange
		// authenticates nobody, so this default is appropriate for a loopback,
		// a LAN and a private match, and is not an answer for a public
		// interface. A ban list, an allow list, a matchmaker's session token
		// and "friends only" are all games' answers and none of them is this
		// module's to invent — the same argument `Authority::SetInterest` is
		// built on.
		//
		// @param policy Called as `policy(Applicant)`. An empty policy is the
		//        default above.
		void SetAdmission(AdmissionPolicy policy);

		// Whether this listener can admit anybody at all.
		//
		// `false` when the operating system refused the entropy the challenge
		// secret is drawn from, in which case every admission is refused and
		// counted. Fail-closed: a listener that carried on with a guessable
		// secret would report healthy admissions while the challenge protected
		// nothing.
		//
		// @return `true` when the challenge secret was drawn.
		bool Admitting() const {
			return Cookie_.has_value();
		}

		// Takes everything waiting on the transport.
		//
		// Runs the admission exchange for an address with no session, and hands
		// everything else to the session that owns it. Call before `Publish`,
		// so an acknowledgement that arrived this tick is counted before this
		// tick's delta is built.
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

			// Peers refused because every slot was taken.
			//
			// The bound doing its job. Counted rather than logged per datagram:
			// a peer that keeps trying would otherwise write a log line per
			// packet, which is its own denial of service.
			uint64_t Turned = 0;

			// Challenges issued to a peer that had not answered one.
			//
			// **Each of these allocated nothing.** A number here that climbs
			// without `Admitted` following it is somebody probing the port, and
			// the honest reading is that it is costing this process one HMAC
			// and one datagram each rather than a slot each.
			uint64_t Challenged = 0;

			// Admission datagrams refused: not a message, a wrong or expired
			// cookie, a key exchange message this build will not agree with, or
			// a server-to-client message arriving from a client.
			uint64_t Refused = 0;

			// Peers the game's admission policy turned away.
			//
			// Apart from `Refused` on purpose. A refusal is the protocol saying
			// no and a rejection is the game saying no, and an operator reading
			// one number for both cannot tell a broken client from a working
			// ban list.
			uint64_t Rejected = 0;
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

			// The key exchange message this peer was admitted on, so a repeated
			// answer can be told from a different peer at the same address.
			std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

			// The `Welcome` datagram, kept so a lost one can be sent again
			// verbatim. It cannot be rebuilt: the ephemeral secret and the
			// `Sealer` that produced the tag are both gone by design, and a
			// second exchange for a live connection is the thing this must not
			// do.
			std::vector<std::byte> Welcome;
		};

		Peer *Find(const net::Endpoint &from);
		void Greet(const net::Endpoint &from, std::span<const std::byte> datagram, double nowSeconds);
		void Challenge(const net::Endpoint &from, const replication::Hello &hello, double nowSeconds);
		void Accept(const net::Endpoint &from, const replication::Answer &answer, double nowSeconds);
		void Repeat(Peer &peer, const Admission &message);
		void Drop(size_t index);

		net::Transport *Transport_;
		ListenerSettings Settings;
		replication::Authority Authority_;

		// The challenge secret, or nothing when the operating system refused
		// entropy — in which case nobody is admitted. See `Admitting`.
		std::optional<net::Cookie> Cookie_;

		AdmissionPolicy Policy;

		std::vector<Peer> Peers;

		// Reused across ticks so a server polling every frame stops allocating.
		std::vector<std::byte> Datagram;

		// The challenge datagram being sent, reused for the same reason: under
		// somebody probing the port this is the hot path, and it is the one path
		// that must stay cheap.
		std::vector<std::byte> Reply;

		// The next connection index to hand a session, and the generation each
		// slot is on. A connection id is `net`'s, not this module's, and it has
		// the same reuse rule every handle here does.
		uint32_t NextConnection = 1;

		Statistics Stats_;
	};
}
