#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/replication/Authority.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace engine::replication {

	namespace {
		// One message, encoded.
		//
		// Built into a fresh writer per message rather than one shared buffer,
		// because the messages are handed out as a span and a caller reads them
		// after `Publish` returns.
		template <class T> std::vector<std::byte> Encode(const T &message) {
			core::ByteWriter writer;
			WriteMessage(writer, message);
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}
	}

	Authority::Authority(const AuthoritySettings &settings) : Settings_(settings) {}

	void Authority::Replicate(core::Name component) {
		if (!component.IsValid()) {
			return;
		}
		if (std::find(Components.begin(), Components.end(), component) == Components.end()) {
			Components.push_back(component);
		}
	}

	bool Authority::Replicated(core::Name component) const {
		return std::find(Components.begin(), Components.end(), component) != Components.end();
	}

	void Authority::SetInterest(std::function<bool(ClientId, ecs::Entity)> predicate) {
		Interest = std::move(predicate);
	}

	ClientId Authority::Admit() {
		// A freed slot is reused, and its generation moves — so a handle from
		// the client that used to be here stops resolving rather than starting
		// to name the new one.
		for (size_t index = 0; index < Clients.size(); index++) {
			if (Clients[index].Live) {
				continue;
			}

			// Carried across the reset, then bumped. Resetting the whole
			// record first would put the generation back to zero and hand the
			// next client the number the last one had — which is exactly the
			// stale handle this scheme exists to make impossible.
			Client &client = Clients[index];
			const uint32_t generation = client.Generation;

			client = Client{};
			client.Generation = generation + 1;
			client.Live = true;
			return ClientId{static_cast<uint32_t>(index), client.Generation};
		}

		Client client;
		client.Generation = 1;
		client.Live = true;
		Clients.push_back(std::move(client));
		return ClientId{static_cast<uint32_t>(Clients.size() - 1), 1};
	}

	Authority::Client *Authority::Reach(ClientId client) {
		if (!client.IsValid() || client.Index >= Clients.size()) {
			return nullptr;
		}

		Client &found = Clients[client.Index];
		if (!found.Live || found.Generation != client.Generation) {
			return nullptr;
		}
		return &found;
	}

	const Authority::Client *Authority::Reach(ClientId client) const {
		return const_cast<Authority *>(this)->Reach(client);
	}

	bool Authority::Remove(ClientId client) {
		Client *found = Reach(client);
		if (found == nullptr) {
			return false;
		}

		// Emptied rather than merely marked, because a dropped client's known
		// set and pending snapshot are the two largest things it held and a
		// server drops clients all day.
		*found = Client{};
		found->Generation = client.Generation;
		return true;
	}

	size_t Authority::Count() const {
		return static_cast<size_t>(std::count_if(Clients.begin(), Clients.end(), [](const Client &client) {
			return client.Live;
		}));
	}

	bool Authority::Holds(ClientId client) const {
		return Reach(client) != nullptr;
	}

	void Authority::BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick) {
		core::ByteWriter writer;
		if (!store.Save(writer)) {
			// A world that cannot be snapshotted cannot admit anybody, and
			// saying so beats sending a client half a world.
			ENGINE_ERROR("replication: the world cannot be snapshotted, so no client can join it.");
			client.Snapshot.clear();
			client.Sent = 0;
			return;
		}

		client.Snapshot.assign(writer.Bytes().begin(), writer.Bytes().end());
		client.Sent = 0;
		client.SnapshotTick = tick;

		// A snapshot *is* the world, so everything in it is now known. Deltas
		// from here are differences against this.
		client.Known.clear();
		for (const ecs::Entity entity : Visible) {
			client.Known.insert(entity.Id);
		}
	}

	void Authority::BuildComponents(ecs::Store &store, const Client &client, Delta &delta) {
		for (const core::Name name : Components) {
			const ecs::ComponentId id = ecs::Components::Find(name);
			if (!id.IsValid()) {
				// Named but never registered. Silently sending nothing would
				// make a missing registration look like a component that never
				// changes, which is the same symptom as a broken system.
				ENGINE_WARN("replication: '{}' is replicated but not registered here.", name.Text());
				continue;
			}

			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
			if (descriptor.Size > 0 && !descriptor.Serialisable) {
				ENGINE_WARN("replication: '{}' has no serialisation and cannot cross.", name.Text());
				continue;
			}

			ComponentDelta component;
			component.Component = name;

			// Runs, not rows. `EachChangedBatch` hands over adjacent changed
			// rows as a block precisely so a delta is a memcpy per run rather
			// than a copy per entity.
			core::ByteWriter values;
			store.EachChangedRuns(id, [&](const ecs::Entity *entities, void *data, size_t rows) {
				// Filtered to what this client knows. A row it has never
				// been told about arrives in `Created` with its whole
				// value; sending a delta for it too would be sending it
				// twice.
				for (size_t row = 0; row < rows; row++) {
					const ecs::Entity entity = entities[row];
					if (client.Known.find(entity.Id) == client.Known.end()) {
						continue;
					}

					component.Entities.push_back(entity);
					if (descriptor.Size > 0) {
						descriptor.Write(
							values, static_cast<const std::byte *>(data) + row * descriptor.Size, 1
						);
					}
				}
			});

			if (component.Entities.empty()) {
				continue;
			}

			component.Values.assign(values.Bytes().begin(), values.Bytes().end());
			delta.Components.push_back(std::move(component));
		}
	}

	void Authority::Publish(ecs::Store &store, uint64_t tick) {
		Stats_.Messages = 0;
		Stats_.Bytes = 0;
		Stats_.Visible = 0;
		Stats_.Resnapshots = 0;

		for (size_t index = 0; index < Clients.size(); index++) {
			Client &client = Clients[index];
			if (!client.Live) {
				continue;
			}

			const ClientId handle{static_cast<uint32_t>(index), client.Generation};
			client.Outgoing.clear();

			// What this client may see, this tick. Recomputed rather than
			// remembered: interest depends on the world, and a cached answer is
			// wrong exactly when something moved.
			Visible.clear();
			store.EachEntity([this, handle](ecs::Entity entity) {
				if (!Interest || Interest(handle, entity)) {
					Visible.push_back(entity);
				}
			});
			Stats_.Visible += Visible.size();

			// A client this far behind cannot be caught up by deltas it never
			// received. Restarting it is the honest answer and the one that
			// needs no repair path nobody tests.
			const bool adrift = client.Snapshot.empty() && client.Sent == 0 && client.Applied > 0 &&
								tick > client.Applied + Settings_.ResnapshotAfterTicks;
			if (adrift) {
				Stats_.Resnapshots++;
			}

			const bool joining = client.Snapshot.empty() && client.Known.empty() && client.Applied == 0;
			if (joining || adrift) {
				BeginSnapshot(client, store, tick);
			}

			// --- streaming the snapshot ---
			if (client.Sent < client.Snapshot.size()) {
				// Spread across ticks. A ten-megabyte world sent in one tick is
				// ten megabytes into a link sized for a few kilobytes, which
				// drops most of it and starves every other client while it
				// does.
				for (size_t chunk = 0; chunk < Settings_.ChunksPerTick; chunk++) {
					if (client.Sent >= client.Snapshot.size()) {
						break;
					}

					const size_t take = std::min(Settings_.ChunkBytes, client.Snapshot.size() - client.Sent);

					SnapshotChunk piece;
					piece.Tick = client.SnapshotTick;
					piece.TotalBytes = static_cast<uint32_t>(client.Snapshot.size());
					piece.Offset = static_cast<uint32_t>(client.Sent);
					piece.Bytes.assign(
						client.Snapshot.begin() + static_cast<ptrdiff_t>(client.Sent),
						client.Snapshot.begin() + static_cast<ptrdiff_t>(client.Sent + take)
					);

					client.Outgoing.push_back(Encode(piece));
					client.Sent += take;
				}

				if (client.Sent >= client.Snapshot.size()) {
					// Released as soon as it is out. A per-client copy of the
					// world is the largest thing a server holds, and holding it
					// after the last chunk is holding it for nothing.
					client.Snapshot.clear();
					client.Snapshot.shrink_to_fit();
					client.Sent = 0;
				}

				// No delta beside a snapshot. The snapshot already describes
				// this tick, and a delta against it would be applied twice.
				for (const std::vector<std::byte> &message : client.Outgoing) {
					Stats_.Bytes += message.size();
				}
				Stats_.Messages += client.Outgoing.size();
				continue;
			}

			// --- the delta ---
			Delta delta;
			delta.Tick = tick;
			delta.Baseline = client.Applied;

			// Created: visible now, not known before. Also the set that has to
			// carry full values rather than only what changed, because a client
			// has never seen these rows at all.
			for (const ecs::Entity entity : Visible) {
				if (client.Known.find(entity.Id) == client.Known.end()) {
					delta.Created.push_back(entity);
				}
			}

			// Destroyed and forgotten, told apart deliberately. An entity that
			// went out of view still exists, and a client that deleted it would
			// be wrong about the world the moment it came back.
			Forget forget;
			forget.Tick = tick;
			{
				std::vector<uint64_t> visible;
				visible.reserve(Visible.size());
				for (const ecs::Entity entity : Visible) {
					visible.push_back(entity.Id);
				}
				std::sort(visible.begin(), visible.end());

				for (const uint64_t known : client.Known) {
					if (std::binary_search(visible.begin(), visible.end(), known)) {
						continue;
					}
					if (store.Alive(ecs::Entity{known})) {
						forget.Entities.push_back(ecs::Entity{known});
					} else {
						delta.Destroyed.push_back(ecs::Entity{known});
					}
				}
			}

			// Deterministic, because two runs of one server must produce the
			// same bytes — the same rule a snapshot follows, for the same
			// reason. An unordered set is walked in whatever order it likes.
			std::sort(delta.Created.begin(), delta.Created.end(), [](ecs::Entity left, ecs::Entity right) {
				return left.Id < right.Id;
			});
			std::sort(
				delta.Destroyed.begin(), delta.Destroyed.end(), [](ecs::Entity left, ecs::Entity right) {
					return left.Id < right.Id;
				}
			);
			std::sort(
				forget.Entities.begin(), forget.Entities.end(), [](ecs::Entity left, ecs::Entity right) {
					return left.Id < right.Id;
				}
			);

			for (const ecs::Entity entity : delta.Created) {
				client.Known.insert(entity.Id);
			}
			for (const ecs::Entity entity : delta.Destroyed) {
				client.Known.erase(entity.Id);
			}
			for (const ecs::Entity entity : forget.Entities) {
				client.Known.erase(entity.Id);
			}

			BuildComponents(store, client, delta);

			const bool quiet = delta.Created.empty() && delta.Destroyed.empty() && delta.Components.empty();
			if (!quiet) {
				client.Outgoing.push_back(Encode(delta));
			}
			if (!forget.Entities.empty()) {
				client.Outgoing.push_back(Encode(forget));
			}

			for (const std::vector<std::byte> &message : client.Outgoing) {
				Stats_.Bytes += message.size();
			}
			Stats_.Messages += client.Outgoing.size();
		}
	}

	std::span<const std::vector<std::byte>> Authority::Outgoing(ClientId client) const {
		const Client *found = Reach(client);
		return found == nullptr ? std::span<const std::vector<std::byte>>{} : found->Outgoing;
	}

	bool Authority::Receive(ClientId client, std::span<const std::byte> message) {
		Client *found = Reach(client);
		if (found == nullptr) {
			Stats_.Refused++;
			return false;
		}

		core::ByteReader reader(message);
		Message read;
		if (!ReadMessage(reader, read)) {
			Stats_.Refused++;
			return false;
		}

		switch (read.Kind) {
		case MessageKind::Input:
			found->Pending.push_back(std::move(read.Input));
			return true;

		case MessageKind::Applied:
			// Never backwards. A client claiming to have applied a tick older
			// than one it already confirmed is either reordered or lying, and
			// believing it would restart a stream that is working.
			if (read.Applied.Tick > found->Applied) {
				found->Applied = read.Applied.Tick;
			}
			return true;

		case MessageKind::SnapshotChunk:
		case MessageKind::Delta:
		case MessageKind::Forget:
			// Server to client only. A client sending one is a client trying to
			// tell the server what the world is, which is the whole thing
			// authority means it may not do.
			Stats_.Refused++;
			return false;
		}

		Stats_.Refused++;
		return false;
	}

	std::span<const Input> Authority::Inputs(ClientId client) const {
		const Client *found = Reach(client);
		return found == nullptr ? std::span<const Input>{} : found->Pending;
	}

	void Authority::ClearInputs(ClientId client) {
		if (Client *found = Reach(client); found != nullptr) {
			found->Pending.clear();
		}
	}

	Authority::ClientStatus Authority::StatusOf(ClientId client) const {
		const Client *found = Reach(client);
		if (found == nullptr) {
			return {};
		}

		ClientStatus status;
		status.Streaming = found->Sent < found->Snapshot.size();
		status.SnapshotRemaining = found->Snapshot.size() - found->Sent;
		status.Applied = found->Applied;
		status.Known = found->Known.size();
		return status;
	}
}
