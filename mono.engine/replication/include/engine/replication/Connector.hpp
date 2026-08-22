#pragma once

// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/ConnectionStats.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Prediction.hpp>
#include <engine/replication/QuicSession.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/replication/SessionPort.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// How a connector talks to its server.
	//
	// @since v0.3
	struct ConnectorSettings {
		// What discovery said this server accepts, or nothing.
		//
		// **A hint, never a choice, and never the client's own.** There is no
		// flag that selects a transport: a connector opens with QUIC and falls
		// back when the server refuses, and the server is the only end that
		// decides. What this saves is the one refusal round trip - an advert
		// from a datagram-only server means the first attempt can go there
		// directly rather than being turned away first.
		//
		// It is a hint because it arrived on an open UDP port from an address
		// anybody can write, exactly like every other field of an advert. Being
		// wrong costs the round trip it was meant to save and nothing else: the
		// fallback still runs.
		//
		// @since v0.19
		std::optional<net::WireMode> Advertised;

		// How long one transport attempt waits for an answer, in seconds.
		//
		// **Only the silent case waits this long.** A server that refuses says
		// so in one round trip and the fallback happens immediately; this is the
		// deadline for a server that says nothing at all, which is what a
		// firewall or a wrong address looks like.
		//
		// @since v0.19
		double AttemptSeconds = 3.0;

		// How a QUIC session is configured, when the attempt is on QUIC.
		//
		// **`ServerIdentity` is copied into it**, so a caller pins the server in
		// one place whichever wire it is on. Under QUIC that pin becomes the
		// RFC 7250 raw public key the TLS handshake checks; under the datagram
		// wire it is the signature over the admission transcript. Same key,
		// same guarantee, different mechanism.
		//
		// @since v0.19
		QuicSessionSettings Quic;

		// The server's public key, or nothing to accept any server.
		// @since v0.9
		std::optional<assets::PublicKey> ServerIdentity;

		// This client's own signing key, or null to prove nothing.
		// @since v0.9
		const assets::SigningKey *ClientIdentity = nullptr;

		// The link's own settings - framing, timeouts and per-tick budgets.
		SessionSettings Session;

		// How much unacknowledged input this client keeps for replay.
		PredictionSettings Prediction;

		// How often to say the same thing again while the exchange is unfinished.
		//
		double RepeatEverySeconds = 0.25;
	};

	// One connection to an authoritative server, and the replica it feeds.
	//
	// @since v0.3
	class Connector {
	  public:
		// Opens a connection to a server.
		//
		// @param transport  The wire. Borrowed, not owned.
		// @param server     Where the server is.
		// @param nowSeconds The current time.
		// @param settings   How to frame, resend and predict.
		Connector(
			net::Transport &transport,
			const net::Endpoint &server,
			double nowSeconds,
			const ConnectorSettings &settings = {}
		);

		// Takes everything waiting, applies it, and acknowledges.
		//
		// @param store      The replica world. Written into.
		// @param nowSeconds The current time.
		void Poll(ecs::Store &store, double nowSeconds);

		// Advances the link and resets its per-tick budget.
		//
		// @param nowSeconds The current time.
		void Advance(double nowSeconds);

		// Sends what the player did this tick.
		// @param tick       The tick the input was produced for.
		// @param bytes      The game's encoding.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it.
		bool Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds);

		// Sends state for the entities this client owns.
		//
		// **Distinct from `Submit`, and the difference is not cosmetic.** An
		// input is what the player *did* and the server decides what it means;
		// this is what the client says the world *is*, for the part of it the
		// server handed over. So the server checks the sender's right to say it
		// and drops what it does not own - `Authority::SetOwnership` carries the
		// policy, including the half deliberately left to the host.
		//
		// Nothing is predicted or replayed here: an owned entity is simulated by
		// this machine and there is no correction to reconcile against, which is
		// exactly what being the owner means. `Prediction` remains the local
		// player's alone.
		//
		// @param delta      The state to send. `BuildSubmission` in
		//                   `Submission.hpp` builds one from a world.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it.
		// @since v0.13
		bool SubmitState(const Delta &delta, double nowSeconds);

		// Whether the key exchange finished and the server let this client in.
		// @return `true` once admitted.
		bool Admitted() const {
			return Phase == Stage::Admitted;
		}

		// Sends a message this module does not read.
		//
		// `Listener::SendTo`'s twin - see the note there on why widening this
		// pair beats standing up a fourth session type.
		//
		// @param message    The payload.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it, including before admission:
		//         there is no session to carry it on until then, and queueing
		//         one would be an outbox this module deliberately does not keep.
		// @since v0.13
		bool SendUser(std::span<const std::byte> message, double nowSeconds);

		// Hears the messages this module does not read.
		//
		// Called from `Poll`, on the thread that called it.
		//
		// @param handler Called as `handler(payload)`.
		// @since v0.13
		void OnUserMessage(std::function<void(std::span<const std::byte>)> handler);

		// Offers every datagram to somebody else before this connector reads it.
		//
		// `Listener::SetForeign`'s twin, and it exists for exactly the same
		// physical reason: a NAT mapping belongs to a port, so a client that
		// punched a hole on some other socket punched a hole to a socket its
		// server will never send to. Both ends of a peer-to-peer session run
		// their rendezvous traffic over the port the session itself uses.
		//
		// Consulted before the source check, because a rendezvous message
		// arrives from a coordination point rather than from the server.
		//
		// @param handler Called as `handler(datagram, from)`, returning whether
		//        it took the datagram. An empty handler is the default.
		// @since v0.13
		void SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler);

		// Whether the exchange failed and this connector will not retry.
		// @return `true` when the exchange is over and failed.
		bool Rejected() const {
			return Phase == Stage::Refused;
		}

		// Whether the full snapshot has arrived and been applied.
		// @return `true` once joined.
		bool Joined() const {
			return Replica_.Joined();
		}

		// Whether what the server put ahead of the world has arrived.
		//
		// True while `Joined` is still false is the window a loading screen
		// belongs in. See `Replica::Prefaced`.
		//
		// @return `true` once the preface has been applied.
		// @since v0.15
		bool Prefaced() const {
			return Replica_.Prefaced();
		}

		// Called the moment the preface lands, with the replicated store.
		//
		// **`Prefaced()` says it happened; this says when.** A single `Poll`
		// drains the socket and applies every message that was in it, so the
		// preface and the world behind it commonly arrive in one call and the
		// window between them is not visible to anything reading state between
		// polls. Whoever raises a loading screen wants this rather than the flag.
		//
		// @param callback What to run, or `{}` to stop listening.
		// @since v0.15
		void OnPreface(std::function<void(ecs::Store &)> callback) {
			Replica_.OnPreface(std::move(callback));
		}

		// The last tick applied in full.
		//
		// @return The tick, or zero before joining.
		uint64_t Applied() const {
			return Replica_.Applied();
		}

		// The inputs the server has not yet confirmed consuming.
		//
		// @return The unacknowledged inputs, oldest first.
		std::span<const Input> Unconfirmed() const {
			return Prediction_.Pending();
		}

		// Entities the server said to stop drawing.
		//
		// @return The entities, valid until the next `Poll`.
		std::span<const ecs::Entity> Forgotten() const {
			return Replica_.Forgotten();
		}

		// Drops the forgotten list, once the caller has acted on it.
		void ClearForgotten() {
			Replica_.ClearForgotten();
		}

		// Which transport this connection landed on.
		//
		// **Read after `Admitted`, and not a setting.** The server chose it; a
		// caller that wants to show a person which stack they are on, or a suite
		// that wants to prove the fallback happened, reads this.
		//
		// @return The wire the current attempt is on.
		// @since v0.19
		net::WireKind Wire() const {
			return Attempting;
		}

		// How many transports have been tried, including the one in progress.
		//
		// @return One when QUIC connected, two once the fallback has started.
		// @since v0.19
		unsigned Attempts() const {
			return Tried;
		}

		// The link's state machine.
		//
		// **Datagram wire only, and null under QUIC.** A QUIC connection has no
		// `net::Link`: its lifecycle, its acknowledgements and its window are
		// the transport's own, and `docs/QUIC.md` §6 keeps only `BytesPerTick`
		// out of that type. A pointer rather than a reference so the absence is
		// something a caller has to look at rather than something it walks into.
		//
		// @return The link, or null when this connector runs on QUIC.
		net::Link *Link();

		// The counters a debug panel reads, whichever wire this is on.
		//
		// **What `Link()` used to be for**, and it works on both: the bytes, the
		// round trip and the loss are facts about a connection rather than about
		// a framing, so they are refilled from whichever transport is underneath
		// rather than reached for through a type only one of them has.
		//
		// @return The statistics.
		// @since v0.19
		net::ConnectionStats LinkStats() const;

		// What this connection has done.
		//
		// @since v0.3
		struct Statistics {
			// Inbound datagrams this connection would not parse.
			//
			// **Every field of an inbound message is hostile**, so a refusal here
			// is the ordinary outcome of a hostile or corrupt packet rather than
			// a fault to investigate - it is the count *rising steadily* that
			// says something.
			uint64_t Refused = 0;

			// Ticks applied to the replica in full.
			uint64_t Applied = 0;
		};

		// What this connection has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

		// What the replica underneath has seen.
		//
		// @return The replica's statistics.
		const Replica::Statistics &ReplicaStats() const {
			return Replica_.Stats();
		}

		// The most transports one connector will try before giving up.
		//
		// Two, because there are two, and a bound is stated rather than implied
		// by the list happening to be short.
		static constexpr unsigned MAXIMUM_ATTEMPTS = 2;

	  private:
		// How far through the admission exchange this end is.
		//
		enum class Stage : uint8_t {
			Greeting,  ///< The hello is going out; no cookie yet.
			Answering, ///< A cookie arrived; the answer is going out.
			Admitted,  ///< The welcome verified. The link is `Connected`.
			Refused,   ///< The exchange failed. Terminal.
		};

		void Repeat(double nowSeconds);
		void Consume(std::span<const std::byte> datagram, double nowSeconds);
		void Refuse();

		void Begin(net::WireKind wire, double nowSeconds);
		bool Fallback(double nowSeconds, const char *why);
		void Reconsider(double nowSeconds);
		void Landed();

		void Settle(double nowSeconds);

		net::Transport *Transport_;

		// Where the server is. Held here rather than read back off the session,
		// because a fallback replaces the session and the address is the one
		// thing that survives it.
		net::Endpoint Server;

		// Whichever session the attempt in flight produced. See
		// `SessionPort.hpp` for why there are two of them and one interface, and
		// `Begin` for why a fallback replaces it whole rather than reusing it.
		std::unique_ptr<SessionPort> Port;

		// The same object as `Port` when the attempt is on QUIC, and null
		// otherwise.
		QuicSession *Quic = nullptr;

		Replica Replica_;
		Prediction Prediction_;

		ConnectorSettings Settings;

		// Lifetime is limited to the handshake.
		std::optional<net::Handshake> Exchange;

		std::array<std::byte, net::Handshake::MESSAGE_BYTES> Mine{};

		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie_{};

		Stage Phase = Stage::Greeting;

		// Which stack the attempt in flight is on, and how many have been made.
		net::WireKind Attempting = net::WireKind::Quic;
		unsigned Tried = 0;
		double AttemptStartedAt = 0.0;

		// Whether the server said, in as many words, that it does not serve the
		// stack this attempt is on. Kept apart from a timeout because the two
		// want different waits: a refusal is acted on now.
		bool Turned = false;

		double SpokeAt = 0.0;
		bool Spoken = false;

		std::vector<std::byte> Datagram;

		// Whoever else is sharing this socket. See SetForeign.
		std::function<bool(std::span<const std::byte>, const net::Endpoint &)> Foreign;

		// Whoever is carrying something over this link. See OnUserMessage.
		std::function<void(std::span<const std::byte>)> UserMessages;

		Statistics Stats_;
	};
}
