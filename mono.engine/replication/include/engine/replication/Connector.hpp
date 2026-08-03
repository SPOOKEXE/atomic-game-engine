#pragma once

// The replica end: one session to one server, and the store it fills.
//
// The mirror of `Listener`, and here for the same reason — so that the pump is
// written once rather than once per program. What it owns is a `Session`, a
// `Replica` and a `Prediction`, and what it does with them each tick is take
// what arrived, apply it, and send back the acknowledgement and the player's
// input.
//
// **The acknowledgement is not optional and is not the caller's to remember.**
// A server stops resending once a client says what it applied, and a client that
// never says stalls its own stream and then gets re-snapshotted for being
// behind. Sending it from inside `Poll` is what stops that being a line somebody
// can leave out.
//
// **Input goes up, state comes down, and nothing goes sideways.** A replica may
// not write to a bus — `world::Replica` refuses at the call — and this class
// offers no way around it: `Submit` takes opaque bytes and they travel as an
// `Input`, which is the only shape in which the server stays the one that
// decided. `replication/AGENTS.md`.
//
// **Nothing flows until the key exchange has.** `Admission.hpp` has the
// sequence; this end drives the initiator's half of it — a hello, a cookie sent
// straight back, and a welcome whose tag has to verify before the link is
// allowed to carry anything. A welcome that does not verify closes the link
// rather than being accepted with a shrug, because a key exchange that half
// worked is one where somebody rewrote a message in flight.
//
// **The retransmission is this end's**, on a timer, because the responder
// deliberately remembers nothing about a peer that has not answered its
// challenge. A handshake that never finishes is a link sitting in `Connecting`
// until `net::LinkSettings::HandshakeTimeoutSeconds`, which `Advance` enforces.
//
// **Time is passed in, never read.**
//
// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Prediction.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// How a connector talks to its server.
	//
	// @since v0.3
	struct ConnectorSettings {
		// How the session frames and resends.
		SessionSettings Session;

		// How much unacknowledged input prediction keeps.
		PredictionSettings Prediction;

		// How often to say the same thing again while the exchange is unfinished.
		//
		// A hello or an answer rides the unreliable channel and the responder
		// keeps nothing to resend from, so this end is the only thing that can
		// cover a lost one. Short enough that a drop costs a fraction of
		// `net::LinkSettings::HandshakeTimeoutSeconds` rather than the whole of
		// it, long enough that a slow round trip is not mistaken for a loss.
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
		//
		// The bytes are the game's own encoding — this layer does not know what
		// an input is and must not, because a module that knew would need
		// changing for every game.
		//
		// @param tick       The tick the input was produced for.
		// @param bytes      The game's encoding.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it.
		bool Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds);

		// Whether the key exchange finished and the server let this client in.
		//
		// True from the moment the welcome's tag verified. Everything before
		// that point is the exchange; nothing of the world has arrived yet.
		//
		// @return `true` once admitted.
		bool Admitted() const {
			return Phase == Stage::Admitted;
		}

		// Whether the exchange failed and this connector will not retry.
		//
		// A welcome whose tag did not verify, a key exchange message this build
		// will not agree with, or no operating system entropy for an ephemeral
		// key. All three are terminal on purpose: `net::Handshake` is
		// single-use, so a second attempt is a second `Connector`.
		//
		// @return `true` when the exchange is over and failed.
		bool Rejected() const {
			return Phase == Stage::Refused;
		}

		// Whether the full snapshot has arrived and been applied.
		//
		// Until this is true the store holds nothing the server sent, and a
		// caller that drew it would draw an empty world.
		//
		// @return `true` once joined.
		bool Joined() const {
			return Replica_.Joined();
		}

		// The last tick applied in full.
		//
		// @return The tick, or zero before joining.
		uint64_t Applied() const {
			return Replica_.Applied();
		}

		// The inputs the server has not yet confirmed consuming.
		//
		// What prediction would replay. Exposed so a game can replay them; this
		// class does not, because replaying means re-running the game's own
		// simulation and it does not have one.
		//
		// @return The unacknowledged inputs, oldest first.
		std::span<const Input> Unconfirmed() const {
			return Prediction_.Pending();
		}

		// Entities the server said to stop drawing.
		//
		// **Not destroyed.** A client that treated "you cannot see this any
		// more" as "this no longer exists" would delete an entity that is still
		// there and then be wrong the moment it came back into view.
		//
		// @return The entities, valid until the next `Poll`.
		std::span<const ecs::Entity> Forgotten() const {
			return Replica_.Forgotten();
		}

		// Drops the forgotten list, once the caller has acted on it.
		void ClearForgotten() {
			Replica_.ClearForgotten();
		}

		// The link's state machine.
		//
		// @return The link.
		net::Link &Link() {
			return Wire.Link();
		}

		// What this connection has done.
		//
		// @since v0.3
		struct Statistics {
			// Messages the replica refused as malformed, stale or unknown.
			uint64_t Refused = 0;

			// Messages applied.
			uint64_t Applied = 0;
		};

		// What this connection has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		// How far through the admission exchange this end is.
		//
		// Forward only, like every other lifecycle here. A refused exchange
		// stays refused: `net::Handshake` is single-use by design, so retrying
		// would mean a second ephemeral key pair, which is a second
		// `Connector`.
		enum class Stage : uint8_t {
			Greeting,  ///< The hello is going out; no cookie yet.
			Answering, ///< A cookie arrived; the answer is going out.
			Admitted,  ///< The welcome verified. The link is `Connected`.
			Refused,   ///< The exchange failed. Terminal.
		};

		void Repeat(double nowSeconds);
		void Consume(std::span<const std::byte> datagram, double nowSeconds);
		void Refuse();

		// Held as well as handed to the session, because draining is this
		// class's job: a `Session` is told about one datagram at a time and
		// deliberately does not know where they come from.
		net::Transport *Transport_;

		Session Wire;
		Replica Replica_;
		Prediction Prediction_;

		ConnectorSettings Settings;

		// The key agreement, for the life of the exchange and no longer. Taken
		// from once, in `Consume`, and empty afterwards.
		std::optional<net::Handshake> Exchange;

		// This end's key exchange message, copied out of `Exchange` so it
		// outlives it. It is a public key; nothing here is secret.
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> Mine{};

		// The cookie the server issued, repeated in every answer and bound into
		// the welcome's tag.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie_{};

		Stage Phase = Stage::Greeting;

		// When the last hello or answer went out, and whether one ever has.
		double SpokeAt = 0.0;
		bool Spoken = false;

		// Reused across ticks so a client polling every frame stops allocating.
		std::vector<std::byte> Datagram;

		Statistics Stats_;
	};
}
