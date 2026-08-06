#pragma once

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
	// @since v0.3
	struct ClientId {
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		uint32_t Index = INVALID;

		uint32_t Generation = 0;

		// @return `true` when it came from `Admit`.
		bool IsValid() const {
			return Index != INVALID;
		}

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
		size_t ChunkBytes = 1024;

		size_t ChunksPerTick = 8;

		uint64_t ResnapshotAfterTicks = 120;

		size_t MessagesPerTick = 32;

		size_t BytesPerTick = 32 * 1024;

		uint64_t StarvationTicks = 30;
	};

	// How the authority notices that a component's value moved.
	//
	// @since v0.7
	enum class ChangeDetection : uint8_t {
		Observed,

		Signature,
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
		// @param detection How changes are detected.
		void Replicate(core::Name component, ChangeDetection detection = ChangeDetection::Observed);

		// How a replicated component's changes are noticed.
		//
		// @param component The component's registered name.
		// @return Its detector, or `Observed` for one that is not replicated.
		ChangeDetection DetectionFor(core::Name component) const;

		// Whether a component is replicated.
		//
		// @param component The component's registered name.
		// @return `true` when it is sent.
		bool Replicated(core::Name component) const;

		// Decides what a client may see.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity)`.
		void SetInterest(std::function<bool(ClientId, ecs::Entity)> predicate);

		// Scores which entities a client is sent first when not all of them
		// fit.
		//
		// @param score Called as `score(ClientId, ecs::Entity)`. Empty scores
		//        everything the same, which leaves the rotation in sole charge
		//        and is a plain round robin.
		void SetPriority(std::function<float(ClientId, ecs::Entity)> score);

		// Decides what to do with a client's identity claim.
		//
		// @param check Called as `check(ClientId, const Identify &)`.
		// @since v0.9
		void SetIdentityCheck(std::function<bool(ClientId, const Identify &)> check);

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
		// @param store The authoritative world.
		// @param tick  The tick just completed.
		void Publish(ecs::Store &store, uint64_t tick);

		// What to send one client, each entry a whole message.
		//
		// @param client The client to ask about.
		// @return The messages, in the order they should go.
		std::span<const std::vector<std::byte>> Outgoing(ClientId client) const;

		// Hands back a message from `Outgoing` that the transport would not
		// take.
		//
		// @param client The client the message was for.
		// @param index  Its position in `Outgoing`.
		void Unsent(ClientId client, size_t index);

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
			bool Streaming = false;

			size_t SnapshotRemaining = 0;

			uint64_t Applied = 0;

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
			size_t Messages = 0;

			size_t Bytes = 0;

			size_t Visible = 0;

			size_t Resnapshots = 0;

			size_t Refused = 0;

			size_t Deferred = 0;

			uint64_t Stalest = 0;
		};

		// What the last `Publish` did.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		static constexpr size_t NOWHERE = static_cast<size_t>(-1);

		// One entity's value for one component, from the sender's side.
		//
		struct Outstanding {
			uint64_t SentAt = 0;

			uint64_t WaitingSince = 0;

			uint64_t ConsideredAt = 0;
		};

		// One entity leaving or entering a client's known set.
		struct Edit {
			uint64_t Entity = 0;

			bool Restore = false;
		};

		// What one outgoing message moved, so a refusal can move it back.
		//
		struct Carried {
			size_t SnapshotOffset = NOWHERE;

			uint32_t First = 0;
			uint32_t Count = 0;

			bool Values = false;
		};

		struct Client {
			uint32_t Generation = 0;
			bool Live = false;

			std::vector<std::byte> Snapshot;
			size_t Sent = 0;
			uint64_t SnapshotTick = 0;

			std::unordered_set<uint64_t> Known;

			uint64_t Applied = 0;

			uint64_t Streamed = 0;

			uint64_t StreamedBefore = 0;

			std::vector<Input> Pending;
			std::vector<std::vector<std::byte>> Outgoing;

			std::vector<std::unordered_map<uint64_t, Outstanding>> Unconfirmed;

			std::vector<Carried> Carried_;

			std::vector<Edit> Edits;
		};

		// One entity's value for one component, built and waiting for a place
		// in a message.
		//
		struct Candidate {
			uint32_t Entry = 0;

			uint32_t Row = 0;

			ecs::Entity Entity;

			uint64_t WaitingSince = 0;

			float Hint = 0.0f;
		};

		// How much of a delta a packing pass got onto the wire.
		struct Placement {
			size_t Values = 0;
		};

		struct Signature {
			std::unordered_map<uint64_t, uint64_t> Hashes;

			std::vector<uint64_t> Changed;
		};

		Client *Reach(ClientId client);
		const Client *Reach(ClientId client) const;
		void Survey(ecs::Store &store);
		void Resign(ecs::Store &store);
		void BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick);
		void BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick);
		void Prioritise(ClientId client, uint64_t tick);
		Placement Pack(Client &client, const Delta &delta, size_t messageLimit);
		void Record(Client &client, const Placement &placed, uint64_t tick);
		void EmitStructure(Client &client, const Structure &structure);

		AuthoritySettings Settings_;
		std::function<bool(ClientId, ecs::Entity)> Interest;
		std::function<float(ClientId, ecs::Entity)> Priority;
		std::function<bool(ClientId, const Identify &)> IdentityCheck;
		std::vector<core::Name> Components;

		std::vector<ChangeDetection> Detection;

		std::vector<Signature> Signatures;

		std::vector<Client> Clients;
		Statistics Stats_;

		std::vector<ecs::Entity> Visible;
		std::vector<Candidate> Candidates;

		std::vector<uint64_t> Bearing;

		std::vector<ecs::ComponentId> Resolved;

		std::vector<uint32_t> Order;

		std::vector<size_t> Strides;
		std::vector<size_t> SourceSlot;

		std::vector<size_t> OpenEntry;

		// Sorted recovery entries preserve deterministic wire order.
		std::vector<uint64_t> Recovering;

		std::vector<uint64_t> Appearing;
	};
}
