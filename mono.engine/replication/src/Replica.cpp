#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Submission.hpp>

#include <algorithm>

namespace engine::replication {

	const char *Describe(ApplyStatus status) {
		switch (status) {
		case ApplyStatus::Ok:
			return "ok";
		case ApplyStatus::Malformed:
			return "malformed";
		case ApplyStatus::Stale:
			return "stale";
		case ApplyStatus::BadChunk:
			return "bad chunk";
		case ApplyStatus::BadSnapshot:
			return "bad snapshot";
		case ApplyStatus::UnknownComponent:
			return "unknown component";
		}
		return "?";
	}

	size_t Replica::SnapshotOutstanding() const {
		return Assembling ? Outstanding : 0;
	}

	void Replica::ClearForgotten() {
		Forgotten_.clear();
	}

	bool Replica::Count(const replication::Delta &delta) {
		if (!Counting.Counting || delta.Tick > Counting.Tick) {
			if (Counting.Counting && !Counting.Whole) {
				Stats_.Incomplete++;
			}

			Counting = Parts{};
			Counting.Tick = delta.Tick;
			Counting.Counting = true;
		} else if (delta.Tick < Counting.Tick) {
			return false;
		}

		if (delta.Part >= Counting.Held.size()) {
			Counting.Held.resize(static_cast<size_t>(delta.Part) + 1, false);
		}

		Counting.Held[delta.Part] = true;

		if (delta.Final) {
			Counting.Last = delta.Part;
			Counting.Ended = true;
		}

		if (!Counting.Ended) {
			return false;
		}

		for (size_t part = 0; part <= Counting.Last; part++) {
			if (part >= Counting.Held.size() || !Counting.Held[part]) {
				return false;
			}
		}

		Counting.Whole = true;
		return true;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const SnapshotChunk &chunk) {
		ENGINE_PROFILE_CAT("replica.snapshot", core::ProfileCategory::Network);

		// **The stage is part of the buffer's identity, beside the tick.** A join
		// is two blobs taken at one tick, so two of the same length would
		// otherwise be assembled into one another — half a preface spliced under
		// half a world, restored without a complaint from anything.
		if (!Assembling || chunk.Tick != SnapshotTick || chunk.Stage != Stage ||
			Snapshot.size() != chunk.TotalBytes) {
			if (Assembling && chunk.Tick < SnapshotTick) {
				return ApplyStatus::Stale;
			}

			Snapshot.assign(chunk.TotalBytes, std::byte{0});
			Received.assign(chunk.TotalBytes, false);
			Outstanding = chunk.TotalBytes;
			SnapshotTick = chunk.Tick;
			Stage = chunk.Stage;
			Assembling = true;
		}

		const size_t end = static_cast<size_t>(chunk.Offset) + chunk.Bytes.size();
		if (end > Snapshot.size()) {
			return ApplyStatus::BadChunk;
		}

		for (size_t index = 0; index < chunk.Bytes.size(); index++) {
			const size_t at = chunk.Offset + index;
			if (!Received[at]) {
				Received[at] = true;
				Outstanding--;
			}
			Snapshot[at] = chunk.Bytes[index];
		}

		if (Outstanding > 0) {
			return ApplyStatus::Ok;
		}

		// **The preface merges and the world sweeps, and swapping them is the
		// bug.** A preface is a slice of a world, so applying it authoritatively
		// would destroy everything it does not mention — which on a re-snapshot
		// is the entire world the client already holds, wiped a moment before
		// being sent it again.
		const bool preface = Stage == SnapshotStage::Preface;

		core::ByteReader reader(Snapshot);
		if (!store.Apply(reader, preface ? ecs::ApplyMode::Overlay : ecs::ApplyMode::Authoritative)) {
			// Apply through the store's scratch path to avoid partial state.
			ENGINE_ERROR("replication: the joining snapshot could not be restored.");
			Assembling = false;
			Snapshot.clear();
			Received.clear();
			return ApplyStatus::BadSnapshot;
		}

		Assembling = false;
		Snapshot.clear();
		Snapshot.shrink_to_fit();
		Received.clear();

		if (preface) {
			// **The join is not over and `Applied` does not move.** What has
			// arrived is the part a loading screen is made of; acknowledging the
			// tick it was taken at would tell the server this client holds a
			// world it has been sent a corner of.
			Prefaced_ = true;
			Stats_.Prefaces++;
			return ApplyStatus::Ok;
		}

		Counting = Parts{};

		// **A snapshot is the whole world, so nothing in it is half-built.**
		// Anything that was being held is either in these bytes with its parent
		// or is not in them at all, and either way the hold has nothing left to
		// say — keeping it would re-parent an entity the snapshot may have put
		// somewhere else.
		Arriving_.clear();

		Applied_ = SnapshotTick;
		Joined_ = true;
		Stats_.Snapshots++;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Delta &delta) {
		ENGINE_PROFILE_CAT("replica.delta", core::ProfileCategory::Network);

		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		if (delta.Tick < Applied_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		// **The write itself is shared with the inbound direction**, which is
		// what `Submission.hpp` is for: a delta going up the wire is the same
		// bytes as one coming down, and the only difference is whether the
		// sender was allowed to say it. No filter here — the sender is the
		// authority.
		const WriteOutcome outcome = WriteComponents(store, delta);
		if (outcome.Status != ApplyStatus::Ok) {
			return outcome.Status;
		}
		const bool whole = outcome.Whole;

		const bool complete = Count(delta);

		Stats_.Deltas++;

		// **Straight after the write and before anything can draw.** The rows
		// this delta carried may have put an arriving entity into the tree; the
		// hold takes it back out until the rest of its tick is here.
		if (!Arriving_.empty()) {
			HoldArrivals(store);

			// The bound. An entity held past `HOLD_DELTAS` is shown wherever it
			// has got to, because content that is simply missing is worse than
			// content that arrived in two steps — `HOLD_DELTAS` carries why.
			const uint64_t seen = Stats_.Deltas;
			std::vector<Arrival> overdue;
			std::erase_if(Arriving_, [&](const Arrival &arriving) {
				if (seen - arriving.Since < HOLD_DELTAS) {
					return false;
				}
				overdue.push_back(arriving);
				return true;
			});

			for (const Arrival &arriving : overdue) {
				if (arriving.Parent != ecs::NULL_ENTITY) {
					(void)store.SetParent(arriving.Entity, arriving.Parent);
				}
			}
		}

		if (!whole) {
			Stats_.Partial++;
		}
		if (!whole || !complete) {
			return ApplyStatus::Ok;
		}

		// **The tick is whole: everything the sender emitted for it is applied.**
		// That is the moment "all the properties are set", so it is the moment
		// the parents go back on.
		ReleaseArrivals(store);

		Applied_ = delta.Tick;
		return ApplyStatus::Ok;
	}

	void Replica::HoldArrivals(ecs::Store &store) {
		for (Arrival &arriving : Arriving_) {
			if (arriving.Parent != ecs::NULL_ENTITY) {
				// Already held. The server goes on sending this entity's rows
				// while it works through the tick, and re-reading the parent
				// here would read the null this put there.
				continue;
			}

			const ecs::Entity parent = store.ParentOf(arriving.Entity);
			if (parent == ecs::NULL_ENTITY) {
				continue;
			}

			arriving.Parent = parent;
			store.SetParent(arriving.Entity, ecs::NULL_ENTITY);
		}
	}

	void Replica::ReleaseArrivals(ecs::Store &store) {
		for (const Arrival &arriving : Arriving_) {
			if (arriving.Parent == ecs::NULL_ENTITY) {
				// It never had one. A replicated root is an ordinary thing —
				// `Instance.new` with no parent is exactly this — and putting it
				// somewhere would be inventing a tree the server did not send.
				continue;
			}

			// **Destroyed while it was held is not a fault.** The entity can be
			// gone by now: a projectile that lived two ticks, or a spawn the
			// server took back. `SetParent` answering false is the honest
			// outcome and there is nothing to do about it.
			(void)store.SetParent(arriving.Entity, arriving.Parent);
		}
		Arriving_.clear();
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Structure &structure) {
		ENGINE_PROFILE_CAT("replica.structure", core::ProfileCategory::Network);

		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		for (const ecs::Entity entity : structure.Created) {
			store.CreateAt(entity);

			// **Recorded before a single component of it has arrived.** The
			// hold is what stops a half-built entity being drawn — see
			// `HoldArrivals` — and the moment to start holding is the moment the
			// entity exists, because the delta that parents it may be the very
			// next message.
			Arriving_.push_back(Arrival{entity, ecs::NULL_ENTITY, Stats_.Deltas});
		}
		for (const ecs::Entity entity : structure.Destroyed) {
			store.SetParent(entity, ecs::NULL_ENTITY);
			store.Destroy(entity);

			// Nothing to give a parent back to. Left in the list it would be a
			// `SetParent` on a dead handle every time a tick completed.
			std::erase_if(Arriving_, [entity](const Arrival &arriving) { return arriving.Entity == entity; });
		}
		Forgotten_.insert(Forgotten_.end(), structure.Forgotten.begin(), structure.Forgotten.end());

		Stats_.Structures++;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Check(const ecs::Store &store, const replication::GroupSignatures &signatures) {
		ENGINE_PROFILE_CAT("replica.audit", core::ProfileCategory::Network);

		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		// **Every name resolved before anything is hashed**, exactly as
		// `WriteComponents` does and for the same reason: a build that cannot
		// name one of these would hash a different set of leaves and report a
		// disagreement about a component rather than about a value. That is a
		// build mismatch and it says so, rather than becoming an audit that
		// disputes everything for ever.
		std::vector<ecs::ComponentId> components;
		components.reserve(signatures.Components.size());
		for (const core::Name named : signatures.Components) {
			const ecs::ComponentId component = ecs::Components::Find(named);
			if (!component.IsValid()) {
				ENGINE_ERROR(
					"replication: an audit names component '{}', which this build has not registered.",
					named.Text()
				);
				return ApplyStatus::UnknownComponent;
			}
			components.push_back(component);
		}

		Disputing_ = replication::Disputed{};
		Disputing_.Tick = signatures.Tick;

		for (const AuditGroup &group : signatures.Groups) {
			if (AuditDigest(store, components, group.Entities, AuditSide::Replica) == group.Digest) {
				continue;
			}
			Disputing_.Groups.push_back(group.Group);
		}

		Stats_.Audits++;
		Stats_.Mismatched += Disputing_.Groups.size();
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Receive(ecs::Store &store, std::span<const std::byte> message) {
		core::ByteReader reader(message);

		Message read;
		if (!ReadMessage(reader, read)) {
			Stats_.Malformed++;
			return ApplyStatus::Malformed;
		}

		switch (read.Kind) {
		case MessageKind::SnapshotChunk:
			return Apply(store, read.Chunk);

		case MessageKind::Delta:
			return Apply(store, read.Delta);

		case MessageKind::Structure:
			return Apply(store, read.Structure);

		case MessageKind::GroupSignatures:
			return Check(store, read.Signatures);

		case MessageKind::Input:
		case MessageKind::Applied:
		case MessageKind::Identify:
		case MessageKind::Disputed:
		// **`User` is refused here rather than ignored.** Its payload is opaque
		// to this module by design — see `MessageKind::User` — so whoever owns
		// the link peels one off before this point. One arriving here means the
		// caller did not, which is a routing mistake worth counting rather than
		// a message to drop quietly.
		case MessageKind::User:
			Stats_.Malformed++;
			return ApplyStatus::Malformed;
		}

		Stats_.Malformed++;
		return ApplyStatus::Malformed;
	}

	std::vector<std::byte> Replica::Acknowledge() const {
		if (!Joined_) {
			return {};
		}

		core::ByteWriter writer;
		WriteMessage(writer, replication::Applied{Applied_});
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	std::vector<std::byte> Replica::Dispute() {
		if (Disputing_.Groups.empty()) {
			return {};
		}

		core::ByteWriter writer;
		WriteMessage(writer, Disputing_);
		Disputing_ = replication::Disputed{};
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}
}
