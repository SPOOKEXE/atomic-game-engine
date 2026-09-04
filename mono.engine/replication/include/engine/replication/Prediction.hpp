#pragma once

// arch-waiver public-header: forward replication API. Client simulation hosts
// share this complete prediction contract.

// The client's run-ahead, and what happens when the server disagrees.
//
// **The local player and nothing else.** Everything else is interpolated
// authoritative state. Predicting a second entity means predicting what another
// player will do, which is wrong more often than it is right and is visible as
// rubber-banding when it is wrong.
//
// The shape is the standard one and it is worth stating because every part of
// it is load-bearing:
//
//  1. the client produces an input for tick N, applies it locally at once, and
//     keeps it
//  2. the server applies it some time later and sends back the state it
//     produced, stamped with the tick it had consumed up to
//  3. the client rewinds the local player to that state and **replays every
//     input after that tick**, arriving back at the present
//
// Step 3 is why the inputs are kept. Without the replay a correction would drag
// the player back to where they were a round trip ago and then let them walk
// forward again, which is the rubber-band.
//
// **This needs no cross-machine determinism.** The client drifting is expected -
// two machines do not compute the same floats and nothing here assumes they do.
// What matters is that a correction is applied *and then replayed from*, so the
// error is bounded by one round trip rather than accumulating.
//
// @tier L12 · shared

#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::replication {

	// How much run-ahead is kept.
	//
	// @since v0.3
	struct PredictionSettings {
		// The most unacknowledged inputs held.
		//
		// **Bounded, because the server may simply stop acknowledging.** An
		// unbounded history is a memory leak driven by the other end, and the
		// replay cost is linear in it - a client a thousand ticks ahead would
		// spend a whole frame replaying and still be wrong.
		//
		// At the cap the oldest is dropped and counted. Dropping the oldest is
		// right rather than convenient: it is the one the server is most likely
		// to have already consumed.
		size_t MaximumPending = 256;
	};

	// The client's unacknowledged inputs.
	//
	// @since v0.3
	class Prediction {
	  public:
		// Creates a prediction buffer.
		//
		// @param settings How much to keep.
		explicit Prediction(const PredictionSettings &settings = {});

		// Records an input the client has just applied locally.
		//
		// @param tick  The tick it was produced for.
		// @param bytes The game's own encoding. Opaque here: a module that knew
		//              what a game's input was would need changing per game.
		// @return `false` when the buffer was full and the oldest was dropped.
		bool Record(uint64_t tick, std::span<const std::byte> bytes);

		// Drops every input the server has now accounted for.
		//
		// Call with the tick the authoritative state describes, immediately
		// after applying it. What is left is exactly what has to be replayed.
		//
		// @param applied The last tick the server consumed.
		// @return How many inputs were retired.
		size_t Reconcile(uint64_t applied);

		// The inputs to replay, oldest first.
		//
		// @return The pending inputs, valid until the next `Record` or
		//         `Reconcile`.
		std::span<const Input> Pending() const {
			return Inputs;
		}

		// How far ahead of the server this client is running, in ticks.
		//
		// The number worth watching. A figure that climbs is a client whose
		// inputs are not reaching the server, and it climbs long before
		// anything looks wrong on screen.
		//
		// @return The pending input count.
		size_t Ahead() const {
			return Inputs.size();
		}

		// Inputs dropped because the buffer was full.
		//
		// Not zero means the server stopped acknowledging for longer than the
		// buffer covers, and the client has predicted from an input the server
		// will never see. Visible rather than silent, because the symptom on
		// screen - a player who slides - has a dozen other possible causes.
		//
		// @return The dropped count.
		uint64_t Dropped() const {
			return Dropped_;
		}

		// Forgets everything. For a rejoin, where the old inputs describe a
		// world that no longer exists.
		void Clear();

	  private:
		PredictionSettings Settings_;
		std::vector<Input> Inputs;
		uint64_t Dropped_ = 0;
	};
}
