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
// **Time is passed in, never read.**
//
// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Prediction.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>

#include <cstdint>
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
		// Held as well as handed to the session, because draining is this
		// class's job: a `Session` is told about one datagram at a time and
		// deliberately does not know where they come from.
		net::Transport *Transport_;

		Session Wire;
		Replica Replica_;
		Prediction Prediction_;

		// Reused across ticks so a client polling every frame stops allocating.
		std::vector<std::byte> Datagram;

		Statistics Stats_;
	};
}
