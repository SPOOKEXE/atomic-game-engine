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
		// **Copied into a scratch world first, and this is not an
		// optimisation.** Saving the authoritative store directly sends the
		// whole of it: every component whether or not it is replicated, every
		// entity whether or not the client may see it, and every resource —
		// which is where the bus outbox, the DataStore records and the world's
		// own bookkeeping live. That contradicts the opt-in rule this module is
		// built on, where `Replicate` names what a client receives and the
		// default is nothing.
		//
		// It also sent `DirtyBits`. Change bits are the sender's own
		// bookkeeping, and a receiving process that never observed anything has
		// no such component registered — so the snapshot named a component that
		// build did not have and the join failed outright. In-process that
		// never showed, because both halves shared one component table; it took
		// a real second process to surface it.
		//
		// The copy costs the size of the visible world, once per join, which is
		// already the expensive operation being spread over ticks below.
		ecs::Store scratch("replica");

		for (const ecs::Entity entity : Visible) {
			// At the sender's exact index *and* generation. A handle stored
			// inside a component has to mean the same entity on both machines,
			// and re-allocating in order would quietly renumber the world.
			scratch.CreateAt(entity);
		}

		for (const core::Name name : Components) {
			const ecs::ComponentId id = ecs::Components::Find(name);
			if (!id.IsValid()) {
				ENGINE_WARN("replication: '{}' is replicated but not registered here.", name.Text());
				continue;
			}

			for (const ecs::Entity entity : Visible) {
				if (const void *value = store.GetComponent(entity, id); value != nullptr) {
					scratch.SetComponent(entity, id, value);
				}
			}
		}

		core::ByteWriter writer;
		if (!scratch.Save(writer)) {
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

		// And nothing is outstanding any more: the snapshot carries the current
		// value of everything in it, so a pending resend would be resending a
		// value the client is about to be told anyway.
		for (std::unordered_map<uint64_t, uint64_t> &unconfirmed : client.Unconfirmed) {
			unconfirmed.clear();
		}
	}

	void Authority::EmitDelta(Client &client, const Delta &delta) {
		// **A delta has to fit a datagram, and one for a whole world does not.**
		// The snapshot was chunked from the start and the delta was not, so a
		// world of thirty-two entities already built a 1365-byte message against
		// a 1200-byte payload limit — and `Link::Reserve` refused every one of
		// them. Silently: the refusal is backpressure, which is the right answer
		// for a link over budget and the wrong diagnosis for a message that can
		// never fit. The symptom was a client that joined and then watched a
		// frozen world forever, which is why this went unnoticed in a suite that
		// only ever checked the join.
		//
		// So a tick's delta goes out as however many messages it takes, each
		// under `ChunkBytes` and **each independently applicable**. Not a
		// numbered sequence to reassemble: this is the unreliable channel, and a
		// reassembly that waits for a part which was dropped is a stall on a
		// path whose whole premise is that the next tick is already on its way.
		// Losing a piece costs those entities one tick.
		//
		// The pieces all carry the same tick, which is why `Replica` treats a
		// delta at the tick it has already applied as another part rather than
		// as stale.
		const size_t budget = Settings_.ChunkBytes;

		// What a message costs before any payload: the kind, the version, the
		// tick, the baseline and four counts. Deliberately generous — the
		// consequence of underestimating is a message that does not fit, which
		// is the failure being fixed.
		constexpr size_t OVERHEAD = 64;

		// A component name is written per entry, so an entry is only worth
		// opening if something can follow it.
		constexpr size_t ENTRY_OVERHEAD = 96;

		Delta piece;
		piece.Tick = delta.Tick;
		piece.Baseline = delta.Baseline;
		size_t used = OVERHEAD;

		const auto flush = [&] {
			const bool empty = piece.Created.empty() && piece.Destroyed.empty() && piece.Components.empty();
			if (!empty) {
				client.Outgoing.push_back(Encode(piece));
			}
			piece = Delta{};
			piece.Tick = delta.Tick;
			piece.Baseline = delta.Baseline;
			used = OVERHEAD;
		};

		// Structure first, and in its own pieces when it needs them. A component
		// value for a row the receiver has not created yet is dropped, so
		// creations lead.
		const auto place = [&](std::vector<ecs::Entity> &into, const std::vector<ecs::Entity> &from) {
			for (const ecs::Entity entity : from) {
				if (used + sizeof(uint64_t) > budget) {
					flush();
				}
				into.push_back(entity);
				used += sizeof(uint64_t);
			}
		};
		place(piece.Created, delta.Created);
		place(piece.Destroyed, delta.Destroyed);

		for (const ComponentDelta &component : delta.Components) {
			if (component.Entities.empty()) {
				continue;
			}

			// From the entry itself rather than from the descriptor, because
			// this is the number of bytes that were actually written for each
			// entity — a type with a custom serialiser is not its `sizeof`.
			const size_t stride = component.Values.size() / component.Entities.size();
			const size_t perEntity = sizeof(uint64_t) + stride;

			size_t offset = 0;
			while (offset < component.Entities.size()) {
				if (used + ENTRY_OVERHEAD + perEntity > budget) {
					flush();
				}

				const size_t room = budget - used - ENTRY_OVERHEAD;
				const size_t take = std::min(room / perEntity, component.Entities.size() - offset);
				if (take == 0) {
					// One entity of this component does not fit an empty
					// message. Nothing can be done about it here and dropping it
					// quietly would be a component that never arrives.
					ENGINE_WARN(
						"replication: '{}' is {} bytes and cannot fit a delta message.",
						component.Component.Text(),
						stride
					);
					break;
				}

				ComponentDelta fragment;
				fragment.Component = component.Component;
				fragment.Entities.assign(
					component.Entities.begin() + static_cast<ptrdiff_t>(offset),
					component.Entities.begin() + static_cast<ptrdiff_t>(offset + take)
				);
				fragment.Values.assign(
					component.Values.begin() + static_cast<ptrdiff_t>(offset * stride),
					component.Values.begin() + static_cast<ptrdiff_t>((offset + take) * stride)
				);

				used += ENTRY_OVERHEAD + take * perEntity;
				piece.Components.push_back(std::move(fragment));
				offset += take;
			}
		}

		flush();
	}

	void Authority::BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick) {
		// One map per replicated component, and the list only grows, so an
		// index into it is stable for the life of the server.
		client.Unconfirmed.resize(Components.size());

		for (size_t slot = 0; slot < Components.size(); slot++) {
			const core::Name name = Components[slot];
			std::unordered_map<uint64_t, uint64_t> &unconfirmed = client.Unconfirmed[slot];

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
					unconfirmed[entity.Id] = tick;
					if (descriptor.Size > 0) {
						descriptor.Write(
							values, static_cast<const std::byte *>(data) + row * descriptor.Size, 1
						);
					}
				}
			});

			// **Everything sent earlier and not yet confirmed, again.** The run
			// walk above is the fast path and covers what moved this tick; this
			// is the recovery path and covers what moved on a tick whose
			// datagram did not arrive. Read one at a time rather than as runs,
			// because the current value is wanted and not the one that was
			// current when the change happened — and because on a link that is
			// not dropping anything this loop finds nothing to do.
			for (auto entry = unconfirmed.begin(); entry != unconfirmed.end();) {
				const uint64_t sentAt = entry->second;
				const ecs::Entity entity{entry->first};

				if (sentAt == tick) {
					// Already in this delta, from the run walk.
					++entry;
					continue;
				}

				if (client.Known.find(entity.Id) == client.Known.end() || !store.Alive(entity)) {
					// Gone or no longer visible. `Destroyed` and `Forget` say so
					// on their own; holding a resend for it would be resending a
					// value for a row the client has been told to drop.
					entry = unconfirmed.erase(entry);
					continue;
				}

				const void *value = store.GetComponent(entity, id);
				if (value == nullptr) {
					entry = unconfirmed.erase(entry);
					continue;
				}

				component.Entities.push_back(entity);
				if (descriptor.Size > 0) {
					descriptor.Write(values, static_cast<const std::byte *>(value), 1);
				}

				entry->second = tick;
				++entry;
			}

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

			BuildComponents(store, client, delta, tick);

			const bool quiet = delta.Created.empty() && delta.Destroyed.empty() && delta.Components.empty();
			if (!quiet) {
				EmitDelta(client, delta);
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

				// Retire what this confirms. An entry sent at or before the tick
				// the client says it applied has arrived, so it stops being
				// resent — and an entry that has not yet gone out at all, which
				// is the zero, is not confirmed by anything.
				for (std::unordered_map<uint64_t, uint64_t> &unconfirmed : found->Unconfirmed) {
					for (auto entry = unconfirmed.begin(); entry != unconfirmed.end();) {
						const bool confirmed = entry->second != 0 && entry->second <= found->Applied;
						entry = confirmed ? unconfirmed.erase(entry) : std::next(entry);
					}
				}
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
