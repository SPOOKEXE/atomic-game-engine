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

		core::ByteReader reader(Snapshot);
		if (!store.Apply(reader, ecs::ApplyMode::Authoritative)) {
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

		Counting = Parts{};

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

		bool whole = true;

		for (size_t index = 0; index < delta.Components.size(); index++) {
			const ComponentDelta &component = delta.Components[index];
			const ecs::ComponentId id = resolved[index];
			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);

			core::ByteReader values(component.Values);
			std::vector<std::byte> scratch(descriptor.Size);

			for (const ecs::Entity entity : component.Entities) {
				if (descriptor.Size == 0) {
					store.SetComponent(entity, id, nullptr);
					continue;
				}

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

				if (store.Alive(entity)) {
					store.SetComponent(entity, id, scratch.data());
				} else {
					whole = false;
				}
				descriptor.Destruct(scratch.data(), 1);
			}
		}

		const bool complete = Count(delta);

		Stats_.Deltas++;

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

		if (!Joined_) {
			Stats_.Stale++;
			return ApplyStatus::Stale;
		}

		for (const ecs::Entity entity : structure.Created) {
			store.CreateAt(entity);
		}
		for (const ecs::Entity entity : structure.Destroyed) {
			store.SetParent(entity, ecs::NULL_ENTITY);
			store.Destroy(entity);
		}
		Forgotten_.insert(Forgotten_.end(), structure.Forgotten.begin(), structure.Forgotten.end());

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
		case MessageKind::Identify:
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
