#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Authority.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace engine::replication {

	namespace {
		template <class T> std::vector<std::byte> Encode(const T &message) {
			core::ByteWriter writer;
			WriteMessage(writer, message);
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}

		uint64_t HashBytes(const void *bytes, size_t count) {
			constexpr uint64_t OFFSET = 1469598103934665603ull;
			constexpr uint64_t PRIME = 1099511628211ull;

			const auto *cursor = static_cast<const unsigned char *>(bytes);
			uint64_t hash = OFFSET;
			for (size_t index = 0; index < count; index++) {
				hash ^= cursor[index];
				hash *= PRIME;
			}
			return hash;
		}

		constexpr size_t MESSAGE_OVERHEAD = 64;

		constexpr size_t ENTRY_OVERHEAD = 96;

		size_t WireBytes(const ecs::TypeDescriptor &descriptor) {
			return descriptor.Wire.Present() ? descriptor.Wire.Size : descriptor.Size;
		}

		void WriteValue(core::ByteWriter &writer, const ecs::TypeDescriptor &descriptor, const void *value) {
			if (descriptor.Wire.Present()) {
				descriptor.Wire.Write(writer, value, 1);
				return;
			}
			descriptor.Write(writer, value, 1);
		}

		constexpr size_t LARGEST_CHUNK = net::Packet::MAXIMUM_MESSAGE_BYTES - MESSAGE_OVERHEAD;
	}

	Authority::Authority(const AuthoritySettings &settings) : Settings_(settings) {
		if (Settings_.ChunkBytes > LARGEST_CHUNK) {
			ENGINE_WARN(
				"replication: a chunk of {} bytes cannot fit a sealed datagram, so it is capped at {}.",
				Settings_.ChunkBytes,
				LARGEST_CHUNK
			);
			Settings_.ChunkBytes = LARGEST_CHUNK;
		}

		if (Settings_.Audit.EntitiesPerGroup == 0 || Settings_.Audit.GroupsPerAudit == 0) {
			Settings_.Audit.Enabled = false;
		}

		if (Settings_.Audit.EveryTicks == 0) {
			Settings_.Audit.EveryTicks = 1;
		}

		if (Settings_.Audit.GroupsPerAudit > MAXIMUM_AUDIT_GROUPS) {
			Settings_.Audit.GroupsPerAudit = MAXIMUM_AUDIT_GROUPS;
		}

		if (Settings_.MessagesPerTick > MAXIMUM_PARTS) {
			ENGINE_WARN(
				"replication: {} delta messages a tick is more than the {} parts a tick may be split "
				"into, so it is capped.",
				Settings_.MessagesPerTick,
				MAXIMUM_PARTS
			);
			Settings_.MessagesPerTick = MAXIMUM_PARTS;
		}
	}

	void Authority::Replicate(core::Name component, ChangeDetection detection) {
		if (!component.IsValid()) {
			return;
		}

		const auto at = std::find(Components.begin(), Components.end(), component);
		if (at != Components.end()) {
			Detection[static_cast<size_t>(std::distance(Components.begin(), at))] = detection;
			return;
		}

		Components.push_back(component);
		Detection.push_back(detection);
		Suppressors.emplace_back();
		Signatures.emplace_back();
	}

	void Authority::SuppressWhenTagged(core::Name component, core::Name tag) {
		// **Only a component already declared can be filtered.** The alternative
		// is remembering a filter for a name nothing sends, which would apply
		// silently the day somebody replicated that component for an unrelated
		// reason — a row going missing with nothing in this file naming it.
		const auto at = std::find(Components.begin(), Components.end(), component);
		if (at == Components.end()) {
			return;
		}

		Suppressors[static_cast<size_t>(std::distance(Components.begin(), at))] = tag;
	}

	bool Authority::Replicated(core::Name component) const {
		return std::find(Components.begin(), Components.end(), component) != Components.end();
	}

	ChangeDetection Authority::DetectionFor(core::Name component) const {
		const auto at = std::find(Components.begin(), Components.end(), component);
		if (at == Components.end()) {
			return ChangeDetection::Observed;
		}
		return Detection[static_cast<size_t>(std::distance(Components.begin(), at))];
	}

	void Authority::SetInterest(std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> predicate) {
		Interest = std::move(predicate);
	}

	void Authority::SetIdentityCheck(std::function<bool(ClientId, const Identify &)> check) {
		IdentityCheck = std::move(check);
	}

	void Authority::SetOwnership(std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> predicate) {
		Ownership = std::move(predicate);
	}

	void Authority::SetPriority(std::function<float(ClientId, ecs::Entity)> score) {
		Priority = std::move(score);
	}

	void Authority::SetPreface(std::function<bool(ecs::Entity, const ecs::Store &)> predicate) {
		Preface = std::move(predicate);
	}

	ClientId Authority::Admit() {
		for (size_t index = 0; index < Clients.size(); index++) {
			if (Clients[index].Live) {
				continue;
			}

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

	void Authority::Survey(ecs::Store &store) {
		// **`Resolved` and `ResolvedNames` are one list in two spellings, filled
		// in one pass.** The audit puts an *ordinal* in every leaf and the names
		// on the wire, so the two ends agree only while both spellings skip the
		// same unregistered slots — and two loops is two chances for them not
		// to. See `Audit.hpp`.
		Resolved.clear();
		ResolvedNames.clear();
		for (const core::Name name : Components) {
			if (const ecs::ComponentId id = ecs::Components::Find(name); id.IsValid()) {
				Resolved.push_back(id);
				ResolvedNames.push_back(name);
			}
		}

		// **Indexed by slot, so it is filled for every slot including the ones
		// that resolve to nothing.** `Resolved` above is a compacted list — it
		// skips a component this process has not registered — and using its
		// indices for anything slot-shaped is how the wrong component gets
		// filtered. See `SuppressWhenTagged`.
		ResolvedSuppressors.assign(Components.size(), ecs::ComponentId{});
		for (size_t slot = 0; slot < Suppressors.size(); slot++) {
			if (Suppressors[slot].IsValid()) {
				ResolvedSuppressors[slot] = ecs::Components::Find(Suppressors[slot]);
			}
		}

		Bearing.clear();
		store.EachEntity([this, &store](ecs::Entity entity) {
			for (const ecs::ComponentId id : Resolved) {
				if (store.HasComponent(entity, id)) {
					Bearing.push_back(entity.Id);
					return;
				}
			}
		});

		std::sort(Bearing.begin(), Bearing.end());

		Resign(store);
	}

	void Authority::Resign(ecs::Store &store) {
		for (size_t slot = 0; slot < Components.size(); slot++) {
			if (Detection[slot] != ChangeDetection::Signature) {
				continue;
			}

			Signature &signature = Signatures[slot];
			signature.Changed.clear();

			const ecs::ComponentId id = ecs::Components::Find(Components[slot]);
			if (!id.IsValid()) {
				continue;
			}

			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
			if (descriptor.Size == 0) {
				continue;
			}

			if (!descriptor.Trivial) {
				ENGINE_WARN(
					"replication: '{}' is not trivially copyable and cannot be signed; "
					"observe it instead.",
					Components[slot].Text()
				);
				continue;
			}

			size_t present = 0;
			for (const uint64_t id64 : Bearing) {
				const void *value = store.GetComponent(ecs::Entity{id64}, id);
				if (value == nullptr) {
					continue;
				}
				present++;

				const uint64_t hash = HashBytes(value, descriptor.Size);
				const auto [entry, fresh] = signature.Hashes.try_emplace(id64, hash);
				if (fresh) {
					signature.Changed.push_back(id64);
					continue;
				}
				if (entry->second != hash) {
					entry->second = hash;
					signature.Changed.push_back(id64);
				}
			}

			if (signature.Hashes.size() == present) {
				continue;
			}

			for (auto entry = signature.Hashes.begin(); entry != signature.Hashes.end();) {
				const ecs::Entity entity{entry->first};
				if (!store.Alive(entity) || store.GetComponent(entity, id) == nullptr) {
					entry = signature.Hashes.erase(entry);
					continue;
				}
				++entry;
			}
		}
	}

	size_t Authority::Owed(const Client &client) {
		size_t owed = 0;
		for (const Staged &staged : client.Snapshots) {
			owed += staged.Bytes.size() - staged.Sent;
		}
		return owed;
	}

	std::vector<std::byte>
	Authority::Capture(ecs::Store &store, std::span<const ecs::Entity> entities) const {
		ecs::Store scratch("replica");

		for (const ecs::Entity entity : entities) {
			scratch.CreateAt(entity);
		}

		core::ByteWriter compact;
		std::vector<std::byte> decoded;

		for (const core::Name name : Components) {
			const ecs::ComponentId id = ecs::Components::Find(name);
			if (!id.IsValid()) {
				ENGINE_WARN("replication: '{}' is replicated but not registered here.", name.Text());
				continue;
			}

			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
			const bool quantised = descriptor.Wire.Present() && descriptor.Size > 0;
			decoded.assign(quantised ? descriptor.Size : 0, std::byte{0});

			for (const ecs::Entity entity : entities) {
				const void *value = store.GetComponent(entity, id);
				if (value == nullptr) {
					continue;
				}

				if (!quantised) {
					scratch.SetComponent(entity, id, value);
					continue;
				}

				compact.Clear();
				descriptor.Wire.Write(compact, value, 1);

				core::ByteReader reader(compact.Bytes());
				descriptor.DefaultConstruct(decoded.data(), 1);
				descriptor.Wire.Read(reader, decoded.data(), 1);
				scratch.SetComponent(entity, id, decoded.data());
				descriptor.Destruct(decoded.data(), 1);
			}
		}

		core::ByteWriter writer;
		if (!scratch.Save(writer)) {
			return {};
		}
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	void Authority::BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick) {
		// **Both blobs are taken at one tick, from one world, in one call.** The
		// alternative — build the world's only once the preface has gone — would
		// give the two halves of a join two different ticks and a window in
		// which the world moved between them, which is a client whose first
		// complete view never existed.
		Preceding.clear();
		if (Preface) {
			for (const ecs::Entity entity : Visible) {
				if (Preface(entity, store)) {
					Preceding.push_back(entity);
				}
			}
		}

		Staged &preface = client.Snapshots[static_cast<size_t>(SnapshotStage::Preface)];
		Staged &world = client.Snapshots[static_cast<size_t>(SnapshotStage::World)];

		preface = Staged{};
		world = Staged{};

		world.Bytes = Capture(store, Visible);
		if (world.Bytes.empty()) {
			ENGINE_ERROR("replication: the world cannot be snapshotted, so no client can join it.");
			return;
		}
		world.Tick = tick;

		if (!Preceding.empty()) {
			// A preface that could not be written is dropped rather than
			// retried: it is the same components out of the same store the world
			// blob has just been written from, so a failure here is that
			// failure, and a join that arrives in one blob is better than one
			// that does not arrive.
			preface.Bytes = Capture(store, Preceding);
			preface.Tick = tick;
		}

		client.Known.clear();
		for (const ecs::Entity entity : Visible) {
			client.Known.insert(entity.Id);
		}

		for (std::unordered_map<uint64_t, Outstanding> &unconfirmed : client.Unconfirmed) {
			unconfirmed.clear();
		}
	}

	void Authority::StreamSnapshot(Client &client) {
		// **One blob per tick, and that is what makes the ordering a rule rather
		// than a likelihood.** Chunks go out in the order this list is built,
		// but a refusal is per message — so a preface chunk the link turned away
		// beside a world chunk it took would be resent *behind* bytes it was
		// supposed to precede. Two blobs never share an outgoing list, so no
		// refusal can reorder them, and the whole cost is one tick at the seam
		// of a join that already spans many.
		for (size_t stage = 0; stage < STAGES; stage++) {
			Staged &staged = client.Snapshots[stage];
			if (staged.Sent >= staged.Bytes.size()) {
				continue;
			}

			for (size_t chunk = 0; chunk < Settings_.ChunksPerTick; chunk++) {
				if (staged.Sent >= staged.Bytes.size()) {
					break;
				}

				const size_t take = std::min(Settings_.ChunkBytes, staged.Bytes.size() - staged.Sent);

				SnapshotChunk piece;
				piece.Stage = static_cast<SnapshotStage>(stage);
				piece.Tick = staged.Tick;
				piece.TotalBytes = static_cast<uint32_t>(staged.Bytes.size());
				piece.Offset = static_cast<uint32_t>(staged.Sent);
				piece.Bytes.assign(
					staged.Bytes.begin() + static_cast<ptrdiff_t>(staged.Sent),
					staged.Bytes.begin() + static_cast<ptrdiff_t>(staged.Sent + take)
				);

				Carried carried;
				carried.SnapshotOffset = staged.Sent;
				carried.Stage = piece.Stage;

				client.Outgoing.push_back(Encode(piece));
				client.Carried_.push_back(carried);
				staged.Sent += take;
			}
			return;
		}
	}

	void Authority::Prioritise(ClientId client, uint64_t tick) {
		if (Priority) {
			for (Candidate &candidate : Candidates) {
				const float hint = Priority(client, candidate.Entity);

				candidate.Hint = std::isfinite(hint) ? hint : 0.0f;
			}
		}

		const uint64_t deadline = Settings_.StarvationTicks;
		const std::vector<Candidate> &candidates = Candidates;

		std::sort(Order.begin(), Order.end(), [&candidates, tick, deadline](uint32_t left, uint32_t right) {
			const Candidate &first = candidates[left];
			const Candidate &second = candidates[right];
			const uint64_t waitedFirst = tick - first.WaitingSince;
			const uint64_t waitedSecond = tick - second.WaitingSince;

			const bool urgentFirst = waitedFirst >= deadline;
			const bool urgentSecond = waitedSecond >= deadline;
			if (urgentFirst != urgentSecond) {
				return urgentFirst;
			}

			if (!urgentFirst && first.Hint != second.Hint) {
				return first.Hint > second.Hint;
			}
			if (waitedFirst != waitedSecond) {
				return waitedFirst > waitedSecond;
			}

			if (first.Entity.Id != second.Entity.Id) {
				return first.Entity.Id < second.Entity.Id;
			}
			return first.Entry < second.Entry;
		});
	}

	Authority::Placement Authority::Pack(Client &client, const Delta &delta, size_t messageLimit) {
		const size_t budget = Settings_.ChunkBytes;

		// **The lower of what the path will carry and what this module is
		// willing to produce.** `SetAllowance` carries the argument; the shape
		// worth noting here is that it is a `min` and not a replacement — a host
		// that never wires a link keeps `SIZE_MAX` and the configured number
		// alone decides, and a host that does still cannot be talked past its
		// own ceiling by a controller that has read the path optimistically.
		const size_t spend = std::min(Settings_.BytesPerTick, client.AllowanceBytes);

		Placement placed;
		size_t messages = 0;
		size_t bytes = 0;
		size_t used = MESSAGE_OVERHEAD;
		size_t rows = 0;

		Delta piece;
		piece.Tick = delta.Tick;
		piece.Baseline = delta.Baseline;
		OpenEntry.assign(delta.Components.size(), NOWHERE);

		Delta emitted;
		size_t emittedAt = NOWHERE;

		const auto flush = [&]() -> bool {
			if (!piece.Components.empty()) {
				piece.Part = static_cast<uint16_t>(messages);
				piece.Final = false;

				std::vector<std::byte> encoded = Encode(piece);
				if (messages >= messageLimit || bytes + encoded.size() > spend) {
					return false;
				}

				bytes += encoded.size();
				messages++;
				placed.Values += rows;

				emittedAt = client.Outgoing.size();
				client.Outgoing.push_back(std::move(encoded));

				Carried carried;
				carried.Values = true;
				client.Carried_.push_back(carried);
				emitted = std::move(piece);
			}

			piece = Delta{};
			piece.Tick = delta.Tick;
			piece.Baseline = delta.Baseline;
			used = MESSAGE_OVERHEAD;
			rows = 0;
			OpenEntry.assign(delta.Components.size(), NOWHERE);
			return true;
		};

		bool room = true;
		for (size_t position = 0; room && position < Order.size(); position++) {
			const Candidate &candidate = Candidates[Order[position]];
			const size_t perEntity = sizeof(uint64_t) + candidate.Bytes;

			size_t entry = OpenEntry[candidate.Entry];
			size_t needs = perEntity + (entry == NOWHERE ? ENTRY_OVERHEAD : 0);

			if (used + needs > budget) {
				if (!(room = flush())) {
					break;
				}
				entry = NOWHERE;
				needs = perEntity + ENTRY_OVERHEAD;
			}

			if (entry == NOWHERE) {
				entry = piece.Components.size();
				OpenEntry[candidate.Entry] = entry;

				ComponentDelta opened;
				opened.Component = delta.Components[candidate.Entry].Component;
				piece.Components.push_back(std::move(opened));
			}

			const ComponentDelta &source = delta.Components[candidate.Entry];
			ComponentDelta &into = piece.Components[entry];
			into.Entities.push_back(candidate.Entity);
			into.Values.insert(
				into.Values.end(),
				source.Values.begin() + static_cast<ptrdiff_t>(candidate.Offset),
				source.Values.begin() + static_cast<ptrdiff_t>(candidate.Offset + candidate.Bytes)
			);

			used += needs;
			rows++;
		}

		if (room) {
			flush();
		}

		if (emittedAt != NOWHERE) {
			emitted.Final = true;
			client.Outgoing[emittedAt] = Encode(emitted);
		}

		return placed;
	}

	void Authority::EmitStructure(Client &client, const Structure &structure) {
		const size_t room =
			Settings_.ChunkBytes > MESSAGE_OVERHEAD ? Settings_.ChunkBytes - MESSAGE_OVERHEAD : 0;
		const size_t perMessage = std::max<size_t>(1, room / sizeof(uint64_t));

		const std::array<const std::vector<ecs::Entity> *, 3> lists{
			&structure.Created, &structure.Destroyed, &structure.Forgotten
		};

		const std::array<bool, 3> restores{false, true, true};

		for (size_t list = 0; list < lists.size(); list++) {
			const std::vector<ecs::Entity> &from = *lists[list];

			for (size_t offset = 0; offset < from.size(); offset += perMessage) {
				const size_t take = std::min(perMessage, from.size() - offset);
				const auto first = from.begin() + static_cast<ptrdiff_t>(offset);
				const auto last = from.begin() + static_cast<ptrdiff_t>(offset + take);

				Structure piece;
				piece.Tick = structure.Tick;
				if (list == 0) {
					piece.Created.assign(first, last);
				} else if (list == 1) {
					piece.Destroyed.assign(first, last);
				} else {
					piece.Forgotten.assign(first, last);
				}

				Carried carried;
				carried.First = static_cast<uint32_t>(client.Edits.size());
				carried.Count = static_cast<uint32_t>(take);
				for (auto entity = first; entity != last; ++entity) {
					client.Edits.push_back(Edit{entity->Id, restores[list]});
				}

				client.Outgoing.push_back(Encode(piece));
				client.Carried_.push_back(carried);
			}
		}
	}

	void Authority::EmitAudit(const ecs::Store &store, ClientId handle, Client &client, uint64_t tick) {
		if (!Settings_.Audit.Enabled || Resolved.empty() || client.Known.empty()) {
			return;
		}
		if (client.AuditedAt != 0 && tick < client.AuditedAt + Settings_.Audit.EveryTicks) {
			return;
		}

		client.AuditedAt = tick;

		Auditing.clear();
		Auditing.reserve(client.Known.size());
		for (const uint64_t known : client.Known) {
			Auditing.push_back(ecs::Entity{known});
		}
		std::sort(Auditing.begin(), Auditing.end(), [](ecs::Entity left, ecs::Entity right) {
			return left.Id < right.Id;
		});

		// **Only what this client has already acknowledged, and that exclusion
		// is the whole reason the audit can be exact.** An entity with an
		// unconfirmed entry is one the delta path is still correcting, so
		// comparing it would report a disagreement about a value that is on its
		// way. What is left is state both ends should hold identically, which
		// is what makes a mismatch mean something rather than mean "late".
		//
		// An entity this client *owns* is excluded for the mirror-image reason:
		// under v0.13 ownership the client's copy is the newer one between
		// submissions, so the server is the side that is behind.
		const auto settled = [&client](ecs::Entity entity) {
			for (const std::unordered_map<uint64_t, Outstanding> &unconfirmed : client.Unconfirmed) {
				if (unconfirmed.find(entity.Id) != unconfirmed.end()) {
					return false;
				}
			}
			return true;
		};

		// **An entity whose rows the receiver derives is left out whole, and
		// that is why nothing about suppression crosses.** `SuppressWhenTagged`
		// stops a component's deltas for a tagged row precisely because the far
		// side recomputes it, so the two ends are *meant* to disagree about it
		// — and a receiver told to skip it would need the tag replicated to it
		// and would report every character in the world as a mismatch on the
		// day somebody forgot. Membership is on the wire, so the cheap answer
		// is simply not to name the entity.
		const auto derives = [this, &store](ecs::Entity entity) {
			for (const ecs::ComponentId tag : ResolvedSuppressors) {
				if (tag.IsValid() && store.HasComponent(entity, tag)) {
					return true;
				}
			}
			return false;
		};

		const size_t wanted =
			static_cast<size_t>(Settings_.Audit.GroupsPerAudit) * Settings_.Audit.EntitiesPerGroup;

		std::vector<ecs::Entity> batch;
		batch.reserve(wanted);

		const auto from = std::lower_bound(
			Auditing.begin(), Auditing.end(), client.AuditCursor, [](ecs::Entity left, uint64_t right) {
				return left.Id < right;
			}
		);

		auto cursor = from;
		for (; cursor != Auditing.end() && batch.size() < wanted; ++cursor) {
			if (!store.Alive(*cursor) || !settled(*cursor) || derives(*cursor)) {
				continue;
			}
			if (Ownership && Ownership(handle, *cursor, store)) {
				continue;
			}
			batch.push_back(*cursor);
		}

		// Wraps rather than stalls at the end of the set, so a sweep that ran
		// out of world starts the next one immediately instead of waiting a
		// rotation for the cursor to be reset by something else.
		client.AuditCursor = cursor == Auditing.end() ? 0 : cursor->Id;

		if (batch.empty()) {
			return;
		}

		GroupSignatures signatures;
		signatures.Tick = tick;
		Auditable.clear();

		// **Only the components this batch actually touches.** A host
		// replicating thirty of them would otherwise spend most of the datagram
		// naming the twenty that contributed nothing, and the ordinal in a leaf
		// is a position in this list rather than in the host's table.
		for (size_t slot = 0; slot < Resolved.size(); slot++) {
			const auto present = [&](ecs::Entity entity) {
				return store.HasComponent(entity, Resolved[slot]);
			};
			if (std::any_of(batch.begin(), batch.end(), present)) {
				signatures.Components.push_back(ResolvedNames[slot]);
				Auditable.push_back(Resolved[slot]);
			}
		}

		if (signatures.Components.empty()) {
			return;
		}

		for (uint32_t group = 0; group < Settings_.Audit.GroupsPerAudit; group++) {
			const size_t first = static_cast<size_t>(group) * Settings_.Audit.EntitiesPerGroup;
			if (first >= batch.size()) {
				break;
			}
			const size_t take = std::min<size_t>(Settings_.Audit.EntitiesPerGroup, batch.size() - first);

			AuditGroup entry;
			entry.Group = group;
			entry.Entities.assign(
				batch.begin() + static_cast<ptrdiff_t>(first),
				batch.begin() + static_cast<ptrdiff_t>(first + take)
			);
			entry.Digest = AuditDigest(store, Auditable, entry.Entities, AuditSide::Authority);
			signatures.Groups.push_back(std::move(entry));
		}

		// Trimmed against the encoding rather than an estimate of it. An audit
		// is built once every `EveryTicks` and a message that cannot fit is
		// refused by the link for ever, which is the failure `ChunkBytes` is
		// capped for one level up.
		std::vector<std::byte> encoded = Encode(signatures);
		while (!signatures.Groups.empty() && encoded.size() > Settings_.ChunkBytes) {
			signatures.Groups.pop_back();
			encoded = Encode(signatures);
		}

		if (signatures.Groups.empty()) {
			// Switched off rather than retried, because the next attempt would
			// be the same arithmetic against the same settings — a warning every
			// `EveryTicks` for the life of the process, saying the same thing.
			Settings_.Audit.Enabled = false;
			ENGINE_WARN(
				"replication: an audit of one group does not fit {} bytes, so the audit is off. "
				"Lower AuditSettings::EntitiesPerGroup or raise ChunkBytes.",
				Settings_.ChunkBytes
			);
			return;
		}

		client.Audit = AuditRecord{};
		client.Audit.Tick = tick;
		client.Audit.Groups = signatures.Groups;

		Carried carried;
		carried.Audit = true;

		client.Outgoing.push_back(std::move(encoded));
		client.Carried_.push_back(carried);
		Stats_.Audits++;
	}

	bool Authority::Dispute(Client &into, const replication::Disputed &disputed) {
		// **The limit is enforced here and nothing about it is taken from the
		// client**, which is what `docs/DEFERRED.md` D00015 calls part of the
		// security argument rather than a tuning knob. An answer is only ever
		// an answer: to the one audit this server issued, once, naming groups
		// out of the slice this server chose. A client claiming everything
		// mismatches can therefore ask for exactly the repair the slice it was
		// asked about would have cost — which the server was already prepared
		// to pay, and which the cadence bounds to once every `EveryTicks`.
		if (!Settings_.Audit.Enabled || into.Audit.Tick == 0 || disputed.Tick != into.Audit.Tick) {
			Stats_.DisputesRefused++;
			return false;
		}

		if (into.Audit.Answered) {
			Stats_.DisputesRefused++;
			return false;
		}

		if (disputed.Groups.empty()) {
			Stats_.DisputesRefused++;
			return false;
		}

		// **In the slice, and strictly ascending.** Those two together are what
		// bound the answer to the question: every label names a group this
		// server hashed, and no label can be named twice — so the most an
		// answer can ask for is the repair of exactly the slice the server had
		// already chosen to look at. A list naming one group a hundred times is
		// the cheapest way to ask for a hundred repairs, and it is refused
		// rather than deduplicated, because this module's `AGENTS.md` says a
		// malformed message is never partly applied. No separate length check:
		// these two imply one.
		for (size_t index = 0; index < disputed.Groups.size(); index++) {
			if (disputed.Groups[index] >= into.Audit.Groups.size()) {
				Stats_.DisputesRefused++;
				return false;
			}
			if (index > 0 && disputed.Groups[index] <= disputed.Groups[index - 1]) {
				Stats_.DisputesRefused++;
				return false;
			}
		}

		into.Audit.Answered = true;
		Stats_.Disputed += disputed.Groups.size();

		for (const uint32_t group : disputed.Groups) {
			for (const ecs::Entity entity : into.Audit.Groups[group].Entities) {
				into.Repairing.push_back(entity.Id);
				Stats_.Repaired++;
			}
		}
		return true;
	}

	void Authority::BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick) {
		client.Unconfirmed.resize(Components.size());

		Candidates.clear();
		SourceSlot.clear();

		for (size_t slot = 0; slot < Components.size(); slot++) {
			const core::Name name = Components[slot];
			std::unordered_map<uint64_t, Outstanding> &unconfirmed = client.Unconfirmed[slot];

			for (const uint64_t appearing : Appearing) {
				unconfirmed.emplace(appearing, Outstanding{});
			}

			// **The repair is the recovery walk, not a second way to resend.**
			// An audit that disagreed says nothing about *which* value is
			// wrong, only that something in the group is — so what it does is
			// put every value of that group back into the unconfirmed set the
			// walk below already reads. `emplace` leaves an entry that is
			// already there alone, so a repair cannot restart the clock on
			// something genuinely in flight.
			for (const uint64_t repairing : client.Repairing) {
				if (client.Known.find(repairing) != client.Known.end()) {
					unconfirmed.emplace(repairing, Outstanding{});
				}
			}

			const ecs::ComponentId id = ecs::Components::Find(name);
			if (!id.IsValid()) {
				ENGINE_WARN("replication: '{}' is replicated but not registered here.", name.Text());
				continue;
			}

			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
			if (descriptor.Size > 0 && !descriptor.Serialisable) {
				ENGINE_WARN("replication: '{}' has no serialisation and cannot cross.", name.Text());
				continue;
			}

			// **A coarse guard, and it is about the component rather than about
			// a row.** A type whose *stored* size already exceeds a message can
			// never produce a row that fits, which is worth saying once at the
			// top instead of building rows nothing will take. It is not a bound
			// on the encoded length: a serialiser that writes names writes as
			// many bytes as the text is long, so what a given row costs is only
			// known after `offer` has written it.
			const size_t crossing = WireBytes(descriptor);
			if (MESSAGE_OVERHEAD + ENTRY_OVERHEAD + sizeof(uint64_t) + crossing > Settings_.ChunkBytes) {
				ENGINE_WARN(
					"replication: '{}' is {} bytes stored and cannot fit a delta message.",
					name.Text(),
					crossing
				);
				continue;
			}

			ComponentDelta component;
			component.Component = name;

			const uint32_t entry = static_cast<uint32_t>(delta.Components.size());
			const size_t before = Candidates.size();

			core::ByteWriter values;

			// **The value is written here rather than beside each call, so that
			// one place records where it landed.** A row's encoded length is
			// only known once it has been written — `scene.Visual` and
			// `ecs.InstanceName` both write names, and a name is as long as its
			// text — and `Pack` has to be able to slice any one row back out
			// after the priority sort has reordered them.
			const auto offer = [&](ecs::Entity entity, const void *value) {
				Outstanding &pending = unconfirmed[entity.Id];
				if (pending.WaitingSince == 0) {
					pending.WaitingSince = tick;
				}
				pending.ConsideredAt = tick;

				const size_t at = values.Bytes().size();
				if (descriptor.Size > 0) {
					WriteValue(values, descriptor, value);
				}

				Candidates.push_back(
					Candidate{
						entry,
						static_cast<uint32_t>(at),
						static_cast<uint32_t>(values.Bytes().size() - at),
						entity,
						pending.WaitingSince,
						0.0f
					}
				);
				component.Entities.push_back(entity);
			};

			// The tag that takes this component's rows off the wire per entity,
			// or invalid when this slot has none — which is every slot by
			// default. Read once rather than per row. See `SuppressWhenTagged`.
			const ecs::ComponentId suppressor =
				slot < ResolvedSuppressors.size() ? ResolvedSuppressors[slot] : ecs::ComponentId{};

			// A row the receiver derives for itself. Skipped *after* the
			// interest check and before `offer`, so a suppressed row costs no
			// acknowledgement slot either — an outstanding entry for a row that
			// is never sent is one the recovery pass would chase for ever.
			const auto derivedThere = [&](const ecs::Entity entity) {
				return suppressor.IsValid() && store.HasComponent(entity, suppressor);
			};

			if (Detection[slot] == ChangeDetection::Signature) {
				for (const uint64_t changed : Signatures[slot].Changed) {
					const ecs::Entity entity{changed};
					if (client.Known.find(changed) == client.Known.end()) {
						continue;
					}
					if (derivedThere(entity)) {
						continue;
					}

					const void *value = store.GetComponent(entity, id);
					if (value == nullptr) {
						continue;
					}

					offer(entity, value);
				}
			} else {
				store.EachChangedRuns(id, [&](const ecs::Entity *entities, void *data, size_t rows) {
					for (size_t row = 0; row < rows; row++) {
						const ecs::Entity entity = entities[row];
						if (client.Known.find(entity.Id) == client.Known.end()) {
							continue;
						}
						if (derivedThere(entity)) {
							continue;
						}

						offer(entity, static_cast<const std::byte *>(data) + row * descriptor.Size);
					}
				});
			}

			Recovering.clear();
			for (const std::pair<const uint64_t, Outstanding> &waiting : unconfirmed) {
				if (waiting.second.ConsideredAt != tick) {
					Recovering.push_back(waiting.first);
				}
			}
			std::sort(Recovering.begin(), Recovering.end());

			for (const uint64_t known : Recovering) {
				const ecs::Entity entity{known};

				if (client.Known.find(known) == client.Known.end() || !store.Alive(entity)) {
					unconfirmed.erase(known);
					continue;
				}

				const void *value = store.GetComponent(entity, id);
				if (value == nullptr) {
					unconfirmed.erase(known);
					continue;
				}

				offer(entity, value);
			}

			if (component.Entities.empty()) {
				Candidates.resize(before);
				continue;
			}

			component.Values.assign(values.Bytes().begin(), values.Bytes().end());

			SourceSlot.push_back(slot);
			delta.Components.push_back(std::move(component));
		}
	}

	void Authority::Record(Client &client, const Placement &placed, uint64_t tick) {
		if (placed.Values > 0) {
			client.StreamedBefore = client.Streamed;
			client.Streamed = tick;
		}

		for (size_t position = 0; position < Order.size(); position++) {
			const Candidate &candidate = Candidates[Order[position]];
			Outstanding &pending = client.Unconfirmed[SourceSlot[candidate.Entry]][candidate.Entity.Id];

			if (position < placed.Values) {
				pending.SentAt = tick;
				pending.WaitingSince = 0;
				continue;
			}

			pending.SentAt = 0;
			Stats_.Deferred++;
			Stats_.Stalest = std::max(Stats_.Stalest, tick - pending.WaitingSince);
		}
	}

	void Authority::Publish(ecs::Store &store, uint64_t tick) {
		Stats_.Messages = 0;
		Stats_.Bytes = 0;
		Stats_.Visible = 0;
		Stats_.Resnapshots = 0;
		Stats_.Deferred = 0;
		Stats_.Stalest = 0;
		Stats_.Audits = 0;

		if (Count() == 0) {
			return;
		}

		Survey(store);

		for (size_t index = 0; index < Clients.size(); index++) {
			Client &client = Clients[index];
			if (!client.Live) {
				continue;
			}

			const ClientId handle{static_cast<uint32_t>(index), client.Generation};
			client.Outgoing.clear();
			client.Carried_.clear();
			client.Edits.clear();

			// Released a tick after the cursor reached the end, because `Unsent`
			// is called after `Publish` has returned and may put it back.
			for (Staged &staged : client.Snapshots) {
				if (!staged.Bytes.empty() && staged.Sent >= staged.Bytes.size()) {
					staged.Bytes.clear();
					staged.Bytes.shrink_to_fit();
					staged.Sent = 0;
				}
			}

			Visible.clear();
			store.EachEntity([this, handle, &store](ecs::Entity entity) {
				if (!std::binary_search(Bearing.begin(), Bearing.end(), entity.Id)) {
					return;
				}
				if (!Interest || Interest(handle, entity, store)) {
					Visible.push_back(entity);
				}
			});
			Stats_.Visible += Visible.size();

			const bool adrift = Owed(client) == 0 && client.Applied > 0 && client.Streamed > client.Applied &&
								tick > client.Applied + Settings_.ResnapshotAfterTicks;
			if (adrift) {
				Stats_.Resnapshots++;
			}

			const bool joining = Owed(client) == 0 && client.Known.empty() && client.Applied == 0;
			if (joining || adrift) {
				BeginSnapshot(client, store, tick);
			}

			if (Owed(client) > 0) {
				StreamSnapshot(client);

				for (const std::vector<std::byte> &message : client.Outgoing) {
					Stats_.Bytes += message.size();
				}
				Stats_.Messages += client.Outgoing.size();
				continue;
			}

			Structure structure;
			structure.Tick = tick;

			for (const ecs::Entity entity : Visible) {
				if (client.Known.find(entity.Id) == client.Known.end()) {
					structure.Created.push_back(entity);
				}
			}

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
						structure.Forgotten.push_back(ecs::Entity{known});
					} else {
						structure.Destroyed.push_back(ecs::Entity{known});
					}
				}
			}

			const auto byHandle = [](ecs::Entity left, ecs::Entity right) { return left.Id < right.Id; };
			std::sort(structure.Created.begin(), structure.Created.end(), byHandle);
			std::sort(structure.Destroyed.begin(), structure.Destroyed.end(), byHandle);
			std::sort(structure.Forgotten.begin(), structure.Forgotten.end(), byHandle);

			for (const ecs::Entity entity : structure.Created) {
				client.Known.insert(entity.Id);
			}
			for (const ecs::Entity entity : structure.Destroyed) {
				client.Known.erase(entity.Id);
			}
			for (const ecs::Entity entity : structure.Forgotten) {
				client.Known.erase(entity.Id);
			}

			if (!structure.Created.empty() || !structure.Destroyed.empty() || !structure.Forgotten.empty()) {
				EmitStructure(client, structure);
			}

			Appearing.clear();
			for (const ecs::Entity entity : structure.Created) {
				Appearing.push_back(entity.Id);
			}

			const size_t structureMessages = client.Outgoing.size();
			const size_t structureEdits = client.Edits.size();

			Delta delta;
			delta.Tick = tick;
			delta.Baseline = client.Applied;

			BuildComponents(store, client, delta, tick);
			client.Repairing.clear();

			if (!delta.Components.empty()) {
				Order.resize(Candidates.size());
				for (size_t position = 0; position < Order.size(); position++) {
					Order[position] = static_cast<uint32_t>(position);
				}

				Placement placed = Pack(client, delta, Settings_.MessagesPerTick);

				if (placed.Values < Candidates.size()) {
					client.Outgoing.resize(structureMessages);
					client.Carried_.resize(structureMessages);
					client.Edits.resize(structureEdits);

					Prioritise(handle, tick);
					placed = Pack(client, delta, Settings_.MessagesPerTick);
				}

				Record(client, placed, tick);
			}

			// Last, so that the byte budget turns this away before it turns
			// away anything that matters. A lost audit costs one rotation and a
			// lost delta costs a client its agreement with the world.
			EmitAudit(store, handle, client, tick);

			for (const std::vector<std::byte> &message : client.Outgoing) {
				Stats_.Bytes += message.size();
			}
			Stats_.Messages += client.Outgoing.size();
		}
	}

	void Authority::SetAllowance(ClientId client, size_t bytes) {
		if (Client *found = Reach(client); found != nullptr) {
			found->AllowanceBytes = bytes;
		}
	}

	std::span<const std::vector<std::byte>> Authority::Outgoing(ClientId client) const {
		const Client *found = Reach(client);
		return found == nullptr ? std::span<const std::vector<std::byte>>{} : found->Outgoing;
	}

	void Authority::Unsent(ClientId client, size_t index) {
		Client *found = Reach(client);
		if (found == nullptr || index >= found->Carried_.size()) {
			return;
		}

		const Carried &carried = found->Carried_[index];

		if (carried.Values) {
			found->Streamed = found->StreamedBefore;
		}

		// A question that was never asked may not be answered. Without this a
		// client could name the tick of an audit the link turned away and be
		// believed, which is the one place its answer would not be bounded by
		// what the server had actually chosen to look at.
		if (carried.Audit) {
			found->Audit = AuditRecord{};
		}

		if (carried.SnapshotOffset != NOWHERE) {
			Staged &staged = found->Snapshots[static_cast<size_t>(carried.Stage)];
			staged.Sent = std::min(staged.Sent, carried.SnapshotOffset);
		}

		for (uint32_t at = carried.First; at < carried.First + carried.Count; at++) {
			const Edit &edit = found->Edits[at];
			if (edit.Restore) {
				found->Known.insert(edit.Entity);
			} else {
				found->Known.erase(edit.Entity);
			}
		}
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
			if (read.Applied.Tick > found->Applied) {
				found->Applied = read.Applied.Tick;

				for (std::unordered_map<uint64_t, Outstanding> &unconfirmed : found->Unconfirmed) {
					for (auto entry = unconfirmed.begin(); entry != unconfirmed.end();) {
						const uint64_t sentAt = entry->second.SentAt;
						const bool confirmed = sentAt != 0 && sentAt <= found->Applied;
						entry = confirmed ? unconfirmed.erase(entry) : std::next(entry);
					}
				}
			}
			return true;

		case MessageKind::Identify:
			return !IdentityCheck || IdentityCheck(client, read.Identify);

		case MessageKind::Delta:
			return Submit(*found, std::move(read.Delta));

		case MessageKind::Disputed:
			return Dispute(*found, read.Disputed);

		case MessageKind::GroupSignatures:
			// A client does not audit a server. The digests say what the
			// authority believes this client holds, and a client sending them
			// back up would be a client telling the authority what its own
			// world is — the same line a snapshot is refused on.
			Stats_.Refused++;
			return false;

		case MessageKind::SnapshotChunk:
		case MessageKind::Structure:
			// **Still refused, and the asymmetry is the point.** A delta is a
			// client saying "this is where the thing I own is"; a snapshot or a
			// structure message is a client saying what exists, which is the
			// one thing an authority may never be told.
			Stats_.Refused++;
			return false;

		case MessageKind::User:
			// Opaque to this module by design, so whoever owns the link peels
			// one off before here — `Replica::Receive` refuses it for the same
			// reason. Reaching this is a routing mistake, and counting it is how
			// it becomes visible.
			Stats_.Refused++;
			return false;
		}

		Stats_.Refused++;
		return false;
	}

	bool Authority::Submit(Client &into, Delta &&delta) {
		// **Nothing may be written until somebody has said who owns what.** The
		// alternative default — accept, and restrict later — makes the insecure
		// state the one a host gets by forgetting, which is the shape of most of
		// the bugs this module's `AGENTS.md` is about.
		//
		// The gate is here and the *filter* is in `ApplySubmitted`, which is not
		// an arrangement of convenience: a delta's values are one packed stream
		// in entity order, so dropping an entity means reading its value off the
		// stream and discarding it. Only the component's descriptor knows how
		// long that value is, and that is at the point of the write.
		if (!Ownership) {
			Stats_.Refused++;
			return false;
		}

		// **Older than what this client has already said is refused.** A
		// submission is the client's whole answer for the entities it owns, so
		// an out-of-order one is not a partial update to merge — it is last
		// tick's position arriving after this tick's, and applying it drags the
		// entity backwards on every machine that is watching.
		//
		// Equal is refused too. Two deltas for one tick is either a duplicate,
		// which says nothing new, or a client contradicting itself.
		if (delta.Tick <= into.SubmittedTick) {
			Stats_.Refused++;
			return false;
		}

		// A delta with no components in it says nothing. Counted as refused
		// rather than accepted as a no-op, because a client sending them is a
		// client doing something worth seeing in the numbers.
		if (delta.Components.empty()) {
			Stats_.Refused++;
			return false;
		}

		into.SubmittedTick = delta.Tick;
		into.Submitted.push_back(std::move(delta));
		return true;
	}

	ApplyStatus Authority::ApplySubmitted(ClientId client, ecs::Store &store) {
		Client *found = Reach(client);
		if (found == nullptr || found->Submitted.empty()) {
			return ApplyStatus::Ok;
		}

		// Refuses everything when nothing has been told who owns what. `Submit`
		// gates on this too; this is the half that holds if a host clears the
		// predicate between receiving and applying.
		const auto allow = [this, client, &store](core::Name component, ecs::Entity entity) {
			// **The component check is not the ownership check.** A client may
			// only write something the server was already sending it: a
			// component this authority does not replicate is one the client has
			// no business knowing about, let alone setting, and owning an entity
			// does not grant a name.
			if (!Replicated(component)) {
				return false;
			}
			return Ownership && Ownership(client, entity, store);
		};

		ApplyStatus worst = ApplyStatus::Ok;
		for (const Delta &delta : found->Submitted) {
			const WriteOutcome outcome = WriteComponents(store, delta, allow);
			Stats_.Unowned += outcome.Refused;
			if (outcome.Status != ApplyStatus::Ok) {
				worst = outcome.Status;
			}
		}

		found->Submitted.clear();
		return worst;
	}

	std::span<const Delta> Authority::Submitted(ClientId client) const {
		const Client *found = Reach(client);
		return found == nullptr ? std::span<const Delta>{} : found->Submitted;
	}

	void Authority::ClearSubmitted(ClientId client) {
		if (Client *found = Reach(client); found != nullptr) {
			found->Submitted.clear();
		}
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
		status.SnapshotRemaining = Owed(*found);
		status.Streaming = status.SnapshotRemaining > 0;
		status.Applied = found->Applied;
		status.Known = found->Known.size();
		return status;
	}
}
