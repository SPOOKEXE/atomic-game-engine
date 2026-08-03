#include <engine/core/Log.hpp>
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

	ApplyStatus Replica::Apply(ecs::Store &store, const SnapshotChunk &chunk) {
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

		Applied_ = SnapshotTick;
		Joined_ = true;
		Stats_.Snapshots++;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Delta &delta) {
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

				descriptor.DefaultConstruct(scratch.data(), 1);
				descriptor.Read(values, scratch.data(), 1);
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

		Stats_.Deltas++;

		// **`Applied` means the last tick applied in full, and this is what makes
		// that true.** The server retires an unconfirmed value once the client
		// says it applied a tick at or after the one the value went out on, so
		// acknowledging a tick whose values were dropped for want of the row
		// confirms something that never landed — and the entity then carries the
		// value it had before, for as long as it does not move again. That is how
		// a creation that arrived late left an entity in the world with none of
		// its components: the creation was resent by the reliable channel and the
		// values were not, because the tick they were in had already been
		// acknowledged.
		if (!whole) {
			Stats_.Partial++;
			return ApplyStatus::Ok;
		}

		Applied_ = delta.Tick;
		return ApplyStatus::Ok;
	}

	ApplyStatus Replica::Apply(ecs::Store &store, const replication::Structure &structure) {
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
