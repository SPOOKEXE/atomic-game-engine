#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/replication/Replica.hpp>

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
		// No default label, so adding a status is a compiler warning here.
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
			// **A newer tick supersedes an older one that never completed, and
			// nothing waits for the part that did not come.** Waiting would be
			// the regression this fix could easily have been: the unreliable
			// channel does not resend, so a part that is gone is gone, and a
			// client that stopped acknowledging until it arrived would stall
			// for ever and earn a re-snapshot — a whole world to repair one
			// value. It is safe to skip because every value the missing part
			// carried is still unconfirmed on the server, so the next tick
			// offers it again and acknowledging *that* tick confirms it.
			if (Counting.Counting && !Counting.Whole) {
				Stats_.Incomplete++;
			}

			Counting = Parts{};
			Counting.Tick = delta.Tick;
			Counting.Counting = true;
		} else if (delta.Tick < Counting.Tick) {
			// A part of a tick already passed over. Its values were still worth
			// applying; its completeness is not, because nothing will
			// acknowledge that tick now.
			return false;
		}

		if (delta.Part >= Counting.Held.size()) {
			// Bounded by `MAXIMUM_PARTS`, which `ReadMessage` has already
			// refused anything above.
			Counting.Held.resize(static_cast<size_t>(delta.Part) + 1, false);
		}

		// A set, so a duplicate sets a bit that is already set rather than
		// counting as a second part.
		Counting.Held[delta.Part] = true;

		if (delta.Final) {
			Counting.Last = delta.Part;
			Counting.Ended = true;
		}

		if (!Counting.Ended) {
			return false;
		}

		// Every position up to the final one, rather than "as many as the final
		// one says". A sender is not trusted to be self-consistent, and a peer
		// that sent parts five and six and then a final part two would
		// otherwise have three arrivals read as parts nought, one and two.
		for (size_t part = 0; part <= Counting.Last; part++) {
			if (part >= Counting.Held.size() || !Counting.Held[part]) {
				return false;
			}
		}

		Counting.Whole = true;
		return true;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const SnapshotChunk &chunk) {
		// One span per message rather than one per frame. A join arrives as a
		// burst of chunks and a steady link carries a delta or two, so a single
		// span over the lot would average the two costs that are worth telling
		// apart — and the burst is the one that drops a frame.
		ENGINE_PROFILE_CAT("replica.snapshot", core::ProfileCategory::Network);

		// A later snapshot supersedes one still arriving. The server only sends
		// a second when it has decided this client cannot be caught up, so
		// finishing the first would be finishing something already abandoned.
		if (!Assembling || chunk.Tick != SnapshotTick || Snapshot.size() != chunk.TotalBytes) {
			if (Assembling && chunk.Tick < SnapshotTick) {
				return ApplyStatus::Stale;
			}

			Snapshot.assign(chunk.TotalBytes, std::byte{0});
			Received.assign(chunk.TotalBytes, false);
			Outstanding = chunk.TotalBytes;
			SnapshotTick = chunk.Tick;
			Assembling = true;
		}

		// Checked against the buffer, not against the claim. `ReadMessage`
		// already refused a chunk running past its own declared total; this
		// refuses one that disagrees with the total already being assembled.
		const size_t end = static_cast<size_t>(chunk.Offset) + chunk.Bytes.size();
		if (end > Snapshot.size()) {
			return ApplyStatus::BadChunk;
		}

		for (size_t index = 0; index < chunk.Bytes.size(); index++) {
			const size_t at = chunk.Offset + index;
			if (!Received[at]) {
				// A duplicate chunk is normal on an unreliable link and must
				// not be counted twice, or the snapshot never completes.
				Received[at] = true;
				Outstanding--;
			}
			Snapshot[at] = chunk.Bytes[index];
		}

		if (Outstanding > 0) {
			return ApplyStatus::Ok;
		}

		core::ByteReader reader(Snapshot);
		if (!store.Apply(reader, ecs::ApplyMode::Authoritative)) {
			// `Store::Apply` reads into a scratch store first, so the live
			// world is untouched. A client left holding its old world is
			// recoverable; one holding half of two is not.
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

		// A snapshot *is* the tick it describes, so whatever parts of an older
		// tick were being counted are about a world this client no longer
		// holds. Dropped rather than counted as a loss: a re-snapshot is the
		// server having given up on the delta stream, not a part going missing.
		Counting = Parts{};

		Applied_ = SnapshotTick;
		Joined_ = true;
		Stats_.Snapshots++;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Delta &delta) {
		ENGINE_PROFILE_CAT("replica.delta", core::ProfileCategory::Network);

		// Nothing before the world exists. A delta against a world that has not
		// arrived describes rows that are not there.
		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		// An unreliable transport reorders, and the newer state is already
		// applied. Not an error, and not a reason to disconnect.
		//
		// **Strictly older, not older-or-equal.** A tick's delta does not fit
		// one datagram once a world has more than a handful of entities, so the
		// authority sends it as several independently applicable messages all
		// stamped with the same tick — see `Authority::EmitDelta`. Refusing the
		// second one as stale is how a world of thirty-two entities received
		// exactly the first fragment of every tick and nothing else. A genuine
		// duplicate re-applies the same values, which is a write that changes
		// nothing rather than a corruption.
		if (delta.Tick < Applied_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		// Resolved before anything is written, so a delta naming a component
		// this build does not have is refused whole rather than applied in
		// part. A world that came back narrower than it was sent would be
		// wrong in a way nothing downstream could see.
		std::vector<ecs::ComponentId> resolved;
		resolved.reserve(delta.Components.size());
		for (const ComponentDelta &component : delta.Components) {
			const ecs::ComponentId id = ecs::Components::Find(component.Component);
			if (!id.IsValid()) {
				ENGINE_ERROR(
					"replication: a delta names component '{}', which this build has not registered.",
					component.Component.Text()
				);
				return ApplyStatus::UnknownComponent;
			}
			resolved.push_back(id);
		}

		// Whether every value in this message reached a row. A value for an
		// entity this client does not hold cannot be applied, and the tick it
		// belongs to is therefore not applied *in full* — see the acknowledgement
		// below.
		bool whole = true;

		for (size_t index = 0; index < delta.Components.size(); index++) {
			const ComponentDelta &component = delta.Components[index];
			const ecs::ComponentId id = resolved[index];
			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);

			core::ByteReader values(component.Values);
			std::vector<std::byte> scratch(descriptor.Size);

			for (const ecs::Entity entity : component.Entities) {
				if (descriptor.Size == 0) {
					// A tag: its presence is its value, and there is nothing to
					// read.
					store.SetComponent(entity, id, nullptr);
					continue;
				}

				// **Through the compact form when the type has one**, which is
				// the same choice `Authority` made when it wrote these bytes.
				// The two are the same registration — `ecs::Components` installs
				// the wire form beside the name — so a build that can resolve
				// the component can decode it, and there is no third state in
				// which one end quantises and the other does not.
				//
				// Never `TypeDescriptor::Read`: that is the file serialisation,
				// it is what a recording is made of, and it must stay lossless.
				descriptor.DefaultConstruct(scratch.data(), 1);
				if (descriptor.Wire.Present()) {
					descriptor.Wire.Read(values, scratch.data(), 1);
				} else {
					descriptor.Read(values, scratch.data(), 1);
				}
				if (values.Failed()) {
					descriptor.Destruct(scratch.data(), 1);
					return ApplyStatus::Malformed;
				}

				// An entity the server named but this client does not hold is
				// dropped rather than created. Creations arrive in a
				// `Structure` message, so one turning up only here is a
				// reorder or a forgery, and inventing a row for it would put
				// an entity in the world that the server does not believe
				// exists.
				if (store.Alive(entity)) {
					store.SetComponent(entity, id, scratch.data());
				} else {
					whole = false;
				}
				descriptor.Destruct(scratch.data(), 1);
			}
		}

		// Counted after the values have been applied, because a part is worth
		// applying whether or not its siblings ever turn up — the numbering
		// decides what is acknowledged and never what is written.
		const bool complete = Count(delta);

		Stats_.Deltas++;

		// **`Applied` means the last tick applied in full, and these two checks
		// are the whole of what makes that true.** The server retires an
		// unconfirmed value once the client says it applied a tick at or after
		// the one the value went out on, so a tick acknowledged short of
		// anything confirms something that never landed — and the entity then
		// carries the value it had before, for as long as it does not move
		// again.
		//
		// Short of a row: a creation resent on the reliable channel arrived late
		// and the entity held none of its components, because the tick its
		// values were in had already been acknowledged.
		//
		// Short of a part: a tick's delta is as many messages as it takes, and
		// acknowledging on the strength of the ones that arrived retired the
		// values in the one that did not. Measured on the reproduction, that was
		// eighteen of forty entities stranded and still eighteen forty ticks
		// later. D00013.
		if (!whole) {
			Stats_.Partial++;
		}
		if (!whole || !complete) {
			return ApplyStatus::Ok;
		}

		Applied_ = delta.Tick;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Structure &structure) {
		ENGINE_PROFILE_CAT("replica.structure", core::ProfileCategory::Network);

		// Nothing before the world exists. The snapshot *is* the structure at the
		// tick it describes, so anything said before it arrived is either already
		// in it or about a world this client does not have.
		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		// **No tick gate, and that is the whole reason this is not part of a
		// delta.** A structural change rides the reliable channel, so it arrives
		// in the order it was sent and is redelivered until it does — which means
		// the copy that finally lands may be describing a tick the replica passed
		// several hundred milliseconds ago. Refusing it as stale is exactly how a
		// creation whose datagram was lost stays lost for the life of the
		// connection, with the server's known set saying the client was told.
		// D00011.
		//
		// A resend that has already been applied is harmless in a way a stale
		// value is not: an entity is created once and destroyed once, and the
		// order those reach here is the order they were decided in.
		for (const ecs::Entity entity : structure.Created) {
			store.CreateAt(entity);
		}
		for (const ecs::Entity entity : structure.Destroyed) {
			// **Unlinked before it is freed, and `Destroy` does not do that.**
			// The server names the entities that died, not the links that named
			// them — so freeing the row on its own leaves a surviving parent
			// still holding it as a child. `EachChild` then hands its body a
			// dead handle and stops there, which quietly loses every sibling
			// behind it: one destroyed part takes the rest of the model off the
			// replica's tree without anything reporting a fault.
			//
			// Not `DestroyInstance`, which would take the subtree with it. Only
			// the server decides what dies here, and a subtree it destroyed is
			// already in this list — entity by entity, in whatever order the
			// handles sort. A dead handle from a resend falls out of `SetParent`
			// as a `false`, so applying one twice stays harmless.
			store.SetParent(entity, ecs::NULL_ENTITY);
			store.Destroy(entity);
		}
		Forgotten_.insert(Forgotten_.end(), structure.Forgotten.begin(), structure.Forgotten.end());

		// **`Applied_` is deliberately not moved.** It names the last tick whose
		// *state* was applied, which is what the server retires unconfirmed
		// values against; a structural message carries no values, so treating it
		// as an applied tick would confirm every value of that tick without one
		// of them having arrived.
		Stats_.Structures++;
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

		case MessageKind::Input:
		case MessageKind::Applied:
			// Client to server only. A server sending one is a server with a
			// bug, and acting on it would hide that.
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
}
