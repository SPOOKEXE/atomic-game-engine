#pragma once

// The server's half: what each client is told, and when.
//
// **A client is admitted, snapshotted, then streamed.** It joins knowing
// nothing, receives one full snapshot of what it may see, and from then on
// receives only what changed. There is no third mode — a client that falls too
// far behind is re-snapshotted rather than repaired, because a repair path is a
// second way to reach a correct world and only one of the two gets tested.
//
// **This produces messages; it does not send them.** Nothing here touches a
// socket, a `net::Link` or a clock. That is what lets the whole of it be driven
// by a test in microseconds, and it is the same argument `net/AGENTS.md` makes
// for `Link` doing no I/O.
//
// **Interest is a predicate, and absence is not destruction.** A client that
// stops being able to see an entity is told to *forget* it, never that it was
// destroyed — a client that conflated the two would delete something still
// there and then be wrong about it the moment it came back into view.
//
// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::replication {

	// One connected client, from the server's point of view.
	//
	// An index and a generation, for the reason every handle in this engine is:
	// a slot is reused and a stale handle must stop resolving rather than start
	// naming somebody else's client.
	//
	// @since v0.3
	struct ClientId {
		// The value no client is given.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The slot.
		uint32_t Index = INVALID;

		// The generation the handle was issued at.
		uint32_t Generation = 0;

		// Whether this handle names a client.
		//
		// @return `true` when it came from `Admit`.
		bool IsValid() const {
			return Index != INVALID;
		}

		// Compares handles.
		//
		// @param other The handle to compare.
		// @return `true` when both name the same client.
		bool operator==(const ClientId &other) const {
			return Index == other.Index && Generation == other.Generation;
		}
	};

	// How the server streams.
	//
	// @since v0.3
	struct AuthoritySettings {
		// The most snapshot bytes put in one chunk.
		//
		// Sized under `net::Packet::MAXIMUM_PAYLOAD_BYTES` with room for this
		// layer's own header. A chunk that did not fit would be refused by the
		// transport, and a snapshot that never finished arriving is a client
		// that never joins.
		size_t ChunkBytes = 1024;

		// How many chunks of a joining client's snapshot go out per tick.
		//
		// **Not all of them.** A ten-megabyte world sent in one tick is ten
		// megabytes into a link sized for a few kilobytes, which drops most of
		// it and starves every other client while it does. Spreading the join
		// costs the joiner a moment and costs everybody else nothing.
		size_t ChunksPerTick = 8;

		// Ticks a client may go without acknowledging before it is re-snapshot.
		//
		// A client this far behind cannot be caught up by deltas it never got.
		uint64_t ResnapshotAfterTicks = 120;
	};

	// What the server owes each client this tick.
	//
	// @since v0.3
	class Authority {
	  public:
		// Creates a server-side replicator.
		//
		// @param settings How to stream.
		explicit Authority(const AuthoritySettings &settings = {});

		// Declares a component as replicated, by name.
		//
		// **Opt in, not opt out.** A world holds components no client has any
		// business receiving — a server-side AI's scratch state, a pending bus
		// request — and a default of "everything" makes leaking one the
		// consequence of forgetting rather than of deciding.
		//
		// @param component The component's registered name.
		void Replicate(core::Name component);

		// Whether a component is replicated.
		//
		// @param component The component's registered name.
		// @return `true` when it is sent.
		bool Replicated(core::Name component) const;

		// Decides what a client may see.
		//
		// Called once per entity per client per tick, so it must be cheap. An
		// empty predicate means every entity is visible to everybody, which is
		// the right answer while worlds are small and the wrong one to leave in
		// place silently — `Statistics::Visible` is what says which you have.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity)`.
		void SetInterest(std::function<bool(ClientId, ecs::Entity)> predicate);

		// Admits a client. It will be sent a full snapshot before any delta.
		//
		// @return Its handle.
		ClientId Admit();

		// Drops a client and forgets what it knew.
		//
		// @param client The client to drop.
		// @return `false` for a handle that never named one, or names one that
		//         has already gone.
		bool Remove(ClientId client);

		// The number of admitted clients.
		//
		// @return The count.
		size_t Count() const;

		// Whether a client is still admitted.
		//
		// @param client The handle to test.
		// @return `true` when it resolves.
		bool Holds(ClientId client) const;

		// Builds this tick's messages for every client.
		//
		// Call once per tick, after the world has ticked and before its change
		// bits are cleared — the bits are the delta source, and clearing them
		// first is how a tick's worth of movement goes missing.
		//
		// @param store The authoritative world.
		// @param tick  The tick just completed.
		void Publish(ecs::Store &store, uint64_t tick);

		// What to send one client, each entry a whole message.
		//
		// Valid until the next `Publish`.
		//
		// @param client The client to ask about.
		// @return The messages, in the order they should go.
		std::span<const std::vector<std::byte>> Outgoing(ClientId client) const;

		// Takes a message from a client.
		//
		// **Every field of it is hostile.** A malformed message is refused and
		// counted; it is never partly applied.
		//
		// @param client  Who sent it.
		// @param message The bytes.
		// @return `false` when it was refused.
		bool Receive(ClientId client, std::span<const std::byte> message);

		// The inputs a client has sent and the game has not yet consumed.
		//
		// @param client The client to ask about.
		// @return The inputs, oldest first.
		std::span<const Input> Inputs(ClientId client) const;

		// Drops the inputs a client sent, once the game has applied them.
		//
		// @param client The client to clear.
		void ClearInputs(ClientId client);

		// What one client's stream is doing.
		//
		// @since v0.3
		struct ClientStatus {
			// Whether the snapshot has finished going out.
			bool Streaming = false;

			// Snapshot bytes still to send.
			size_t SnapshotRemaining = 0;

			// The last tick this client said it applied.
			uint64_t Applied = 0;

			// Entities it currently knows about.
			size_t Known = 0;
		};

		// One client's stream.
		//
		// @param client The client to ask about.
		// @return Its status, or an empty record for an unknown handle.
		ClientStatus StatusOf(ClientId client) const;

		// What the last `Publish` did.
		//
		// @since v0.3
		struct Statistics {
			// Messages built, across every client.
			size_t Messages = 0;

			// Bytes built, across every client.
			size_t Bytes = 0;

			// Entities visible, summed across clients. Compared against the
			// world's own count, this is what says whether interest is
			// filtering anything at all.
			size_t Visible = 0;

			// Clients restarted from a snapshot because they fell too far
			// behind. Not zero is a bandwidth problem, not a bug.
			size_t Resnapshots = 0;

			// Messages refused as malformed since the last reset.
			size_t Refused = 0;
		};

		// What the last `Publish` did.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		struct Client {
			uint32_t Generation = 0;
			bool Live = false;

			// The snapshot being streamed, and how much of it has gone.
			std::vector<std::byte> Snapshot;
			size_t Sent = 0;
			uint64_t SnapshotTick = 0;

			// Entities this client has been told about. What `Created`,
			// `Destroyed` and `Forget` are all differences against.
			std::unordered_set<uint64_t> Known;

			uint64_t Applied = 0;
			std::vector<Input> Pending;
			std::vector<std::vector<std::byte>> Outgoing;
		};

		Client *Reach(ClientId client);
		const Client *Reach(ClientId client) const;
		void BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick);
		void BuildComponents(ecs::Store &store, const Client &client, Delta &delta);

		AuthoritySettings Settings_;
		std::function<bool(ClientId, ecs::Entity)> Interest;
		std::vector<core::Name> Components;
		std::vector<Client> Clients;
		Statistics Stats_;

		// Reused between ticks so a server streaming every frame stops
		// allocating.
		std::vector<ecs::Entity> Visible;
		std::vector<std::byte> Scratch;
	};
}
