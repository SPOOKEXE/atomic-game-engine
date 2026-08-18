#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Authority.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace engine::replication {

	namespace {
		// Multiplies and stops at the maximum rather than wrapping.
		//
		// The refinement window is a tick's reach times a configured factor, and
		// both are settings a host writes. A wrap would turn "look at more rows"
		// into "look at almost none", which reads as the second hook silently
		// not running.
		size_t SaturatingProduct(size_t value, size_t factor) {
			if (factor == 0 || value == 0) {
				return 0;
			}
			return value > SIZE_MAX / factor ? SIZE_MAX : value * factor;
		}

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

	namespace {
		// Where an entity sits, or would sit, in a sorted row list.
		template <class Rows> auto SeatFor(Rows &rows, uint64_t entity) {
			return std::lower_bound(rows.begin(), rows.end(), entity, [](const auto &row, uint64_t id) {
				return row.Entity < id;
			});
		}
	}

	Authority::Outstanding &Authority::OutstandingSet::operator[](uint64_t entity) {
		const auto seat = SeatFor(Rows, entity);
		if (seat != Rows.end() && seat->Entity == entity) {
			return seat->Value;
		}
		return Rows.insert(seat, Row{entity, Outstanding{}})->Value;
	}

	void Authority::OutstandingSet::Emplace(uint64_t entity) {
		const auto seat = SeatFor(Rows, entity);
		if (seat != Rows.end() && seat->Entity == entity) {
			return;
		}
		Rows.insert(seat, Row{entity, Outstanding{}});
	}

	void Authority::OutstandingSet::EmplaceAll(std::span<const uint64_t> entities) {
		if (entities.empty()) {
			return;
		}

		for (const uint64_t entity : entities) {
			Rows.push_back(Row{entity, Outstanding{}});
		}

		// **Stable, and that is what keeps an existing row.** Everything already
		// here was in front of everything appended, so a stable sort leaves a
		// duplicate's existing copy first and `unique` keeps the first of a run.
		// An assigning insert would restart the clock on a value genuinely in
		// flight, which is `Emplace`'s rule and has to survive the bulk form.
		std::stable_sort(Rows.begin(), Rows.end(), [](const Row &left, const Row &right) {
			return left.Entity < right.Entity;
		});
		Rows.erase(
			std::unique(
				Rows.begin(),
				Rows.end(),
				[](const Row &left, const Row &right) { return left.Entity == right.Entity; }
			),
			Rows.end()
		);
	}

	void Authority::OutstandingSet::Erase(uint64_t entity) {
		const auto seat = SeatFor(Rows, entity);
		if (seat != Rows.end() && seat->Entity == entity) {
			Rows.erase(seat);
		}
	}

	void Authority::OutstandingSet::EraseSorted(std::span<const uint64_t> entities) {
		if (entities.empty()) {
			return;
		}

		size_t cursor = 0;
		size_t kept = 0;
		for (size_t index = 0; index < Rows.size(); index++) {
			while (cursor < entities.size() && entities[cursor] < Rows[index].Entity) {
				cursor++;
			}
			if (cursor < entities.size() && entities[cursor] == Rows[index].Entity) {
				continue;
			}
			Rows[kept++] = Rows[index];
		}
		Rows.resize(kept);
	}

	void
	Authority::OutstandingSet::SelectRecovering(uint64_t tick, size_t limit, std::vector<uint64_t> &into) {
		into.clear();
		if (Rows.empty()) {
			return;
		}

		const size_t take = limit == 0 ? Rows.size() : limit;

		size_t index = static_cast<size_t>(SeatFor(Rows, Cursor) - Rows.begin());
		if (index >= Rows.size()) {
			index = 0;
		}

		// Every row is examined even when only `take` are kept, so the walk
		// stops on the first tick where fewer than `take` are outstanding rather
		// than spinning to find them. Examining is a compare; keeping is a
		// serialisation, and it is the second one this bounds.
		for (size_t step = 0; step < Rows.size() && into.size() < take; step++) {
			if (Rows[index].Value.ConsideredAt != tick) {
				into.push_back(Rows[index].Entity);
			}
			index = index + 1 < Rows.size() ? index + 1 : 0;
		}

		Cursor = Rows[index].Entity;
	}

	bool Authority::OutstandingSet::Contains(uint64_t entity) const {
		const auto seat = SeatFor(Rows, entity);
		return seat != Rows.end() && seat->Entity == entity;
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
		// reason - a row going missing with nothing in this file naming it.
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

	void Authority::SetPriorityRefinement(std::function<float(ClientId, ecs::Entity, float)> refine) {
		Refinement = std::move(refine);
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
		ENGINE_PROFILE_CAT("Authority::Survey", core::ProfileCategory::Network);

		{
			ENGINE_PROFILE_CAT("Authority::ResolveComponents", core::ProfileCategory::Network);

			// **`Resolved` and `ResolvedNames` are one list in two spellings, filled
			// in one pass.** The audit puts an *ordinal* in every leaf and the names
			// on the wire, so the two ends agree only while both spellings skip the
			// same unregistered slots - and two loops is two chances for them not
			// to. See `Audit.hpp`.
			Resolved.clear();
			ResolvedNames.clear();
			for (size_t slot = 0; slot < Components.size(); slot++) {
				const ecs::ComponentId id = ecs::Components::Find(Components[slot]);
				if (!id.IsValid()) {
					continue;
				}

				Resolved.push_back(id);
				ResolvedNames.push_back(Components[slot]);

				// **A component declared `Observed` is observed here rather than by
				// whoever built the authority, because the pairing is worth nothing
				// if its two halves can disagree.** `Observed` means "read the
				// store's dirty bits", and a store that is not recording them
				// answers `false` for every row - so a host that declared the
				// detector and forgot `Store::Observe` sends nothing, reports
				// nothing, and looks exactly like a component nobody wrote to. That
				// is the failure `ReplicatedComponent::Detection`'s own comment
				// calls silent in both directions, and there were three hosts to
				// forget it in.
				//
				// `ObserveComponent` is idempotent, so the steady cost is a set
				// lookup per replicated component per tick. The *first* call is not
				// free: it moves every entity already carrying the component into an
				// archetype with somewhere to put the bits, which happens once, on
				// the tick the first client makes this run.
				if (Detection[slot] == ChangeDetection::Observed) {
					store.ObserveComponent(id);
				}
			}

			// **Indexed by slot, so it is filled for every slot including the ones
			// that resolve to nothing.** `Resolved` above is a compacted list - it
			// skips a component this process has not registered - and using its
			// indices for anything slot-shaped is how the wrong component gets
			// filtered. See `SuppressWhenTagged`.
			ResolvedSuppressors.assign(Components.size(), ecs::ComponentId{});
			for (size_t slot = 0; slot < Suppressors.size(); slot++) {
				if (Suppressors[slot].IsValid()) {
					ResolvedSuppressors[slot] = ecs::Components::Find(Suppressors[slot]);
				}
			}
		}

		{
			ENGINE_PROFILE_CAT("Authority::FindBearing", core::ProfileCategory::Network);
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
		}

		{
			ENGINE_PROFILE_CAT("Authority::Resign", core::ProfileCategory::Network);
			Resign(store);
		}
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

			// One span per component, not one for the whole loop - `Resign`
			// used to be a single frame no capture could see inside: twenty
			// components signed for one 39%-of-the-tick line gives a reader no
			// way to tell whether that is one expensive component or several
			// cheap ones. The name is stable for the life of the run, so this
			// is the `_STABLE` form rather than the copying one.
			ENGINE_PROFILE_DYNAMIC_STABLE("Authority::Resign::slot", Components[slot].Text(), core::ProfileCategory::Network);

			// This slot's actual carriers, hashed straight out of the column -
			// not `Bearing` filtered down to them and not read back through
			// `GetComponent`. `Bearing` is the union of every replicated
			// component's carriers, and a scene usually declares far more
			// replicated component types than any one entity has: walking
			// `Bearing` here would spend a miss on every entity that does not
			// happen to carry *this* component. `EachRuns` instead visits only
			// the archetype tables that do, chunk by chunk, and hands back the
			// raw column alongside the entities - so a component nothing in the
			// scene carries costs one table lookup and touches no entity at
			// all, and hashing a populated one reads contiguous memory instead
			// of paying a directory lookup and an archetype fetch per entity.
			ResignHashed.clear();
			store.EachRuns(id, [this, &descriptor](const ecs::Entity *entities, void *values, size_t rows) {
				const auto *bytes = static_cast<const std::byte *>(values);
				for (size_t row = 0; row < rows; row++) {
					ResignHashed.emplace_back(
						entities[row].Id, HashBytes(bytes + row * descriptor.Size, descriptor.Size)
					);
				}
			});
			// `EachRuns` visits table by table, not id order, and the merge
			// below needs id order to walk `signature.Hashes` alongside it.
			std::sort(ResignHashed.begin(), ResignHashed.end());

			// A merge against `ResignHashed`, which is now sorted the same way
			// `signature.Hashes` already is. `previous` only ever moves forward,
			// so an entity the candidate set skipped past - it departed since
			// last tick - is left behind rather than copied into `next`, which
			// is what used to need a second pass over the whole map to find.
			std::vector<std::pair<uint64_t, uint64_t>> next;
			next.reserve(signature.Hashes.size());
			size_t previous = 0;
			for (const auto &[id64, hash] : ResignHashed) {
				while (previous < signature.Hashes.size() && signature.Hashes[previous].first < id64) {
					previous++;
				}

				const bool known = previous < signature.Hashes.size() && signature.Hashes[previous].first == id64;
				if (!known || signature.Hashes[previous].second != hash) {
					signature.Changed.push_back(id64);
				}
				next.emplace_back(id64, hash);
			}
			signature.Hashes.swap(next);
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
		// alternative - build the world's only once the preface has gone - would
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

		for (OutstandingSet &unconfirmed : client.Unconfirmed) {
			unconfirmed.Clear();
		}

		// Cleared here rather than where it is read: a world blob carries every
		// value at any size, so whatever the oversized list was asking for is
		// already in the bytes above.
		client.Oversize.clear();
	}

	void Authority::StageOversize(Client &client, ecs::Store &store, uint64_t tick) {
		std::sort(client.Oversize.begin(), client.Oversize.end());
		client.Oversize.erase(
			std::unique(client.Oversize.begin(), client.Oversize.end()), client.Oversize.end()
		);

		Preceding.clear();
		for (const uint64_t named : client.Oversize) {
			const ecs::Entity entity{named};

			// **Still alive and still this client's to see.** The list is built
			// a tick before it is read, and an entity that left interest in
			// between is one this client is about to be told to forget.
			if (store.Alive(entity) && client.Known.find(named) != client.Known.end()) {
				Preceding.push_back(entity);
			}
		}

		client.Oversize.clear();
		if (Preceding.empty()) {
			return;
		}

		// **The preface slot, and it cannot collide with a join.** The caller
		// only reaches here with nothing owed, so both staged blobs are empty -
		// and a join beginning later overwrites this one deliberately, because a
		// world blob carries these entities anyway.
		//
		// **Applied as an overlay, which is the whole reason this can be a slice
		// at all.** `Replica` sweeps for a `World` blob and merges a `Preface`
		// one; a handful of entities applied authoritatively would delete every
		// other row the client holds.
		Staged &staged = client.Snapshots[static_cast<size_t>(SnapshotStage::Preface)];
		staged = Staged{};
		staged.Bytes = Capture(store, Preceding);
		staged.Tick = tick;

		if (staged.Bytes.empty()) {
			ENGINE_ERROR("replication: {} oversized rows could not be captured.", Preceding.size());
		}
	}

	void Authority::StreamSnapshot(Client &client) {
		// **One blob per tick, and that is what makes the ordering a rule rather
		// than a likelihood.** Chunks go out in the order this list is built,
		// but a refusal is per message - so a preface chunk the link turned away
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
		ENGINE_PROFILE_CAT("Authority::Prioritise", core::ProfileCategory::Network);

		if (Priority) {
			ENGINE_PROFILE_CAT("Authority::Score", core::ProfileCategory::Network);

			// **Once per entity rather than once per row.** `SetPriority`'s
			// hook is `(client, entity)` and nothing else, so the four rows an
			// entity with a transform, a motion, a name and a class produces are
			// four questions with one answer. What the repeats cost is whatever
			// the host's lookup costs, and `mono.server`'s includes an occlusion
			// raycast - measured at 69% of a two-hundred-client tick before this
			// existed.
			//
			// `Bearing` is the sorted set of entities carrying a replicated
			// component, so it indexes the table with a binary search and no
			// allocation. A candidate whose entity is not in it - a row for
			// something destroyed since the survey - is scored directly rather
			// than being given somebody else's number.
			//
			// A quiet NaN is the "not yet asked" mark, which is exact because
			// every score stored below has been through `std::isfinite`.
			Scores.assign(Bearing.size(), std::numeric_limits<float>::quiet_NaN());

			for (Candidate &candidate : Candidates) {
				const auto found = std::lower_bound(Bearing.begin(), Bearing.end(), candidate.Entity.Id);
				const bool known = found != Bearing.end() && *found == candidate.Entity.Id;
				const size_t slot = known ? static_cast<size_t>(found - Bearing.begin()) : 0;

				if (known && !std::isnan(Scores[slot])) {
					candidate.Hint = Scores[slot];
					continue;
				}

				const float hint = Priority(client, candidate.Entity);
				candidate.Hint = std::isfinite(hint) ? hint : 0.0f;
				if (known) {
					Scores[slot] = candidate.Hint;
				}
			}
		}

		ENGINE_PROFILE_CAT("Authority::Sort", core::ProfileCategory::Network);

		const uint64_t deadline = Settings_.StarvationTicks;
		const std::vector<Candidate> &candidates = Candidates;

		const auto before = [&candidates, tick, deadline](uint32_t left, uint32_t right) {
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
		};

		// **Only the rows that could reach the wire have to be in order.**
		// `Pack` walks `Order` from the front and stops the moment the budget is
		// spent, so ordering what is past that point is work whose result
		// nothing ever reads - and on a server over its budget the tail is most
		// of the world.
		//
		// The bound is what this tick could carry at its most generous: the
		// smaller of the byte allowance and the message budget, over the least a
		// row can cost. A row past that cannot be reached even if every row in
		// front of it is empty.
		//
		// **The least a row can cost is measured, not assumed.** Eight bytes for
		// an entity handle plus a component that serialises to nothing is a
		// legitimate lower bound and a useless one: at the default budget it
		// makes every candidate in a two-thousand-entity world "reachable", so
		// the partial sort degrades to a full sort on the tick it matters. The
		// shortest row actually built this tick is a lower bound too, it is one
		// pass over lengths `BuildComponents` has already measured, and on a
		// world of transforms it is several times larger.
		//
		// **The first `reachable` are exactly what a full sort would have put
		// there**, because the comparator is a total order - which is the same
		// property `AGENTS.md` requires of it so that two runs of one server
		// produce the same bytes.
		const Client *held = Reach(client);
		const size_t allowance = held != nullptr ? held->AllowanceBytes : SIZE_MAX;
		const size_t spend = std::min(
			std::min(Settings_.BytesPerTick, allowance), Settings_.MessagesPerTick * Settings_.ChunkBytes
		);

		// Straight down `Candidates` rather than through `Order`, which holds
		// exactly one index per candidate. Both answer the same question; only
		// one of them reads memory in the order it is laid out, and this runs
		// per client per tick over every row in the world.
		size_t shortest = SIZE_MAX;
		for (const Candidate &candidate : Candidates) {
			shortest = std::min(shortest, static_cast<size_t>(candidate.Bytes));
		}

		const size_t leastRow = sizeof(uint64_t) + (shortest == SIZE_MAX ? 0 : shortest);
		const size_t reachable = spend / leastRow + 1;

		if (reachable < Order.size()) {
			std::partial_sort(
				Order.begin(), Order.begin() + static_cast<ptrdiff_t>(reachable), Order.end(), before
			);
		} else {
			std::sort(Order.begin(), Order.end(), before);
		}

		Refine(client, reachable, spend, before);
	}

	template <class Before>
	void Authority::Refine(ClientId client, size_t reachable, size_t spend, const Before &before) {
		if (!Refinement || Settings_.PriorityRefinementFactor == 0 || Order.empty()) {
			return;
		}

		ENGINE_PROFILE_CAT("Authority::Refine", core::ProfileCategory::Network);

		// **What the budget really reaches, not the bound the sort used.**
		// `reachable` assumes every row ahead of a row is the shortest row in
		// the tick, which is what a bound has to assume and is nowhere near what
		// a tick sends. The first version of this window used it directly, came
		// out wider than the whole world, and moved the occlusion cost from one
		// profile frame to another without removing any of it.
		//
		// Adding up what the ordered rows actually encode to answers it in one
		// walk of a prefix, out of lengths `BuildComponents` already measured.
		// Per-message overhead is left out, so the count errs wide - which is
		// the safe direction for a window.
		const size_t ordered = std::min(reachable, Order.size());

		size_t carried = 0;
		size_t spent = 0;
		while (carried < ordered && spent < spend) {
			spent += sizeof(uint64_t) + Candidates[Order[carried]].Bytes;
			carried++;
		}

		// **The rows in contention, which is what the second hook is for.**
		// Widening what the tick carries leaves a properly-looked-at row behind
		// every row a refinement demotes. Past the window nothing is asked, and
		// the unrefined score standing in for it is an upper bound - see
		// `SetPriorityRefinement` for why that is the safe direction and what it
		// approximates.
		//
		// **The window is sorted before this runs and re-sorted after**, because
		// a refinement changes the key the first sort used. Only the window is
		// re-sorted: the tail was never in order past `reachable` anyway, and a
		// row in it carries a score nothing refined.
		const size_t window = std::min(
			Order.size(), SaturatingProduct(std::max<size_t>(carried, 1), Settings_.PriorityRefinementFactor)
		);

		if (window > reachable) {
			// The window reaches past what the first sort put in order, so the
			// rows between have to be brought in before they can be refined.
			std::partial_sort(
				Order.begin(), Order.begin() + static_cast<ptrdiff_t>(window), Order.end(), before
			);
		}

		// Per *entity*, exactly as the cheap half is memoised, and for the same
		// reason: the hook is `(client, entity)` and the four rows an entity
		// produces are four questions with one answer.
		Refined.assign(Bearing.size(), std::numeric_limits<float>::quiet_NaN());

		for (size_t position = 0; position < window; position++) {
			Candidate &candidate = Candidates[Order[position]];

			const auto found = std::lower_bound(Bearing.begin(), Bearing.end(), candidate.Entity.Id);
			const bool known = found != Bearing.end() && *found == candidate.Entity.Id;
			const size_t slot = known ? static_cast<size_t>(found - Bearing.begin()) : 0;

			if (known && !std::isnan(Refined[slot])) {
				candidate.Hint = Refined[slot];
				continue;
			}

			const float refined = Refinement(client, candidate.Entity, candidate.Hint);

			// **Clamped to the score it started from.** A refinement that raised
			// one would outrank rows this pass never looked at, which is the one
			// way the window stops being an ordering and starts being a filter.
			// A host that returns a NaN or a larger number gets its own input
			// back rather than a silently reordered stream.
			candidate.Hint = std::isfinite(refined) && refined < candidate.Hint ? refined : candidate.Hint;
			if (known) {
				Refined[slot] = candidate.Hint;
			}
		}

		std::sort(Order.begin(), Order.begin() + static_cast<ptrdiff_t>(window), before);
	}

	Authority::Placement Authority::Pack(Client &client, const Delta &delta, size_t messageLimit) {
		ENGINE_PROFILE_CAT("Authority::Pack", core::ProfileCategory::Network);

		const size_t budget = Settings_.ChunkBytes;

		// **The lower of what the path will carry and what this module is
		// willing to produce.** `SetAllowance` carries the argument; the shape
		// worth noting here is that it is a `min` and not a replacement - a host
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
			for (const OutstandingSet &unconfirmed : client.Unconfirmed) {
				if (unconfirmed.Contains(entity.Id)) {
					return false;
				}
			}
			return true;
		};

		// **An entity whose rows the receiver derives is left out whole, and
		// that is why nothing about suppression crosses.** `SuppressWhenTagged`
		// stops a component's deltas for a tagged row precisely because the far
		// side recomputes it, so the two ends are *meant* to disagree about it
		// - and a receiver told to skip it would need the tag replicated to it
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
			// be the same arithmetic against the same settings - a warning every
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
		// client**, which is what `docs/retired/DEFERRED.md` D00015 calls part of the
		// security argument rather than a tuning knob. An answer is only ever
		// an answer: to the one audit this server issued, once, naming groups
		// out of the slice this server chose. A client claiming everything
		// mismatches can therefore ask for exactly the repair the slice it was
		// asked about would have cost - which the server was already prepared
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
		// server hashed, and no label can be named twice - so the most an
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
			OutstandingSet &unconfirmed = client.Unconfirmed[slot];

			// In bulk, because a joining client's `Appearing` is the whole world
			// and inserting that into a sorted list one entity at a time moves
			// the tail once per entity.
			{
				ENGINE_PROFILE_CAT("Authority::PrepareOutstanding", core::ProfileCategory::Network);
				unconfirmed.EmplaceAll(Appearing);

				// **The repair is the recovery walk, not a second way to resend.**
				// An audit that disagreed says nothing about *which* value is
				// wrong, only that something in the group is - so what it does is
				// put every value of that group back into the unconfirmed set the
				// walk below already reads. `Emplace` leaves an entry that is
				// already there alone, so a repair cannot restart the clock on
				// something genuinely in flight.
				for (const uint64_t repairing : client.Repairing) {
					if (client.Known.find(repairing) != client.Known.end()) {
						unconfirmed.Emplace(repairing);
					}
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
			// only known once it has been written - `scene.Visual` and
			// `ecs.InstanceName` both write names, and a name is as long as its
			// text - and `Pack` has to be able to slice any one row back out
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

				// **A row that cannot fit a message is sent by the other path,
				// and the point is that it is *not* sent by this one.** `Pack`
				// has no smaller unit than a row: too big for a piece, it goes
				// into one anyway, the encoded message is over
				// `net::Packet::MAXIMUM_MESSAGE_BYTES`, `Link::Reserve` refuses
				// it - and a refusal is what ordinary backpressure looks like,
				// so `Unsent` puts it back and the same row is rebuilt and
				// refused every tick for the life of the connection. Four
				// kilobytes of Luau in a `script.Program` reaches that on the
				// tick a game clones a script.
				//
				// A snapshot blob is the bulk path this module already has - it
				// is chunked, it is offset-addressed and it is reassembled - so
				// the entity goes into one through `StageOversize`, which builds
				// the same slice-shaped blob a preface does. Re-snapshotting the
				// whole world was the first answer and cost 81 ticks of
				// streaming an event, during which the client is told about
				// nothing else at all.
				//
				// **The unconfirmed entry is erased and not left**, which is the
				// half that took a suite to find. The recovery walk exists to
				// keep offering a value until the client acknowledges a tick it
				// was in, so an entry for a row no message can hold is one it
				// re-offers on every tick for the life of the connection. The
				// staged blob is what carries it, and nothing is left to chase.
				//
				// The bytes stay in `values` because `core::ByteWriter` does not
				// rewind. Nothing reads them: a row is only ever sliced out
				// through a `Candidate`, and this one produces none.
				const size_t wrote = values.Bytes().size() - at;
				if (MESSAGE_OVERHEAD + ENTRY_OVERHEAD + sizeof(uint64_t) + wrote > Settings_.ChunkBytes) {
					unconfirmed.Erase(entity.Id);
					client.Oversize.push_back(entity.Id);
					Stats_.Oversized++;
					return;
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
			// or invalid when this slot has none - which is every slot by
			// default. Read once rather than per row. See `SuppressWhenTagged`.
			const ecs::ComponentId suppressor =
				slot < ResolvedSuppressors.size() ? ResolvedSuppressors[slot] : ecs::ComponentId{};

			// A row the receiver derives for itself. Skipped *after* the
			// interest check and before `offer`, so a suppressed row costs no
			// acknowledgement slot either - an outstanding entry for a row that
			// is never sent is one the recovery pass would chase for ever.
			const auto derivedThere = [&](const ecs::Entity entity) {
				return suppressor.IsValid() && store.HasComponent(entity, suppressor);
			};

			{
				ENGINE_PROFILE_CAT("Authority::DetectRows", core::ProfileCategory::Network);
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
			}

			// **Already in order, so there is no sort here any more.**
			// `OutstandingSet` holds its rows ascending precisely so this walk
			// does not have to copy every unconfirmed key of every component of
			// every client into a scratch vector and sort it once a tick. On a
			// saturated server the unconfirmed set is the whole world, and that
			// copy was the largest single thing this function did.
			//
			// **And bounded, resuming where it stopped.** See
			// `AuthoritySettings::RecoveryRowsPerTick`: the second largest thing
			// it did was serialise every one of those rows so the priority sort
			// could pick the forty a link would take.
			//
			// Ids rather than an iterator, because `offer` inserts into
			// `unconfirmed` and would invalidate one.
			{
				ENGINE_PROFILE_CAT("Authority::RecoverRows", core::ProfileCategory::Network);
				unconfirmed.SelectRecovering(tick, Settings_.RecoveryRowsPerTick, Recovering);

				// **Dropped in one pass at the end rather than one at a time.** A
				// row leaves the set when the entity has gone, and erasing from a
				// sorted list moves its tail - so a world losing a thousand entities
				// would move that tail a thousand times. `EraseSorted` needs its
				// input ascending and `Recovering` wraps, so this is sorted rather
				// than assumed - it holds only rows with nothing behind them, which
				// is a handful next to the walk that produced them.
				Dropping.clear();
				for (const uint64_t known : Recovering) {
					const ecs::Entity entity{known};

					if (client.Known.find(known) == client.Known.end() || !store.Alive(entity)) {
						Dropping.push_back(known);
						continue;
					}

					const void *value = store.GetComponent(entity, id);
					if (value == nullptr) {
						Dropping.push_back(known);
						continue;
					}

					offer(entity, value);
				}
				std::sort(Dropping.begin(), Dropping.end());
				unconfirmed.EraseSorted(Dropping);
			}

			{
				ENGINE_PROFILE_CAT("Authority::CommitRows", core::ProfileCategory::Network);
				if (component.Entities.empty()) {
					Candidates.resize(before);
					continue;
				}

				component.Values.assign(values.Bytes().begin(), values.Bytes().end());

				SourceSlot.push_back(slot);
				delta.Components.push_back(std::move(component));
			}
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
		// **Instrumented per phase and not per client**, which is the shape a
		// two-hundred-client host forces: `FrameGraph::MAXIMUM_SPANS` is 4096 and
		// a span per client per phase would spend the frame's whole budget on
		// the instrumentation. Every client's visibility walk shares one stack in
		// the folded output, which is the number a reader wants anyway - what one
		// client costs is this over the client count.
		ENGINE_PROFILE_CAT("Authority::Publish", core::ProfileCategory::Network);

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

			{
				ENGINE_PROFILE_CAT("Authority::Interest", core::ProfileCategory::Network);

				// **`Bearing` rather than the whole store, which is the same set
				// by a shorter road.** `Survey` has just built the sorted ids of
				// everything carrying a replicated component, so walking the
				// store and asking whether each entity is in that list is an
				// archetype walk plus a binary search per entity to arrive at a
				// list already in hand - once per client, so on a full server it
				// is that walk two hundred times.
				//
				// It also leaves `Visible` ascending by handle, which the
				// structural pass below takes rather than re-deriving.
				Visible.clear();
				Visible.reserve(Bearing.size());
				for (const uint64_t id : Bearing) {
					const ecs::Entity entity{id};
					if (!Interest || Interest(handle, entity, store)) {
						Visible.push_back(entity);
					}
				}
			}
			Stats_.Visible += Visible.size();

			const bool adrift = Owed(client) == 0 && client.Applied > 0 && client.Streamed > client.Applied &&
								tick > client.Applied + Settings_.ResnapshotAfterTicks;
			if (adrift) {
				Stats_.Resnapshots++;
			}

			const bool joining = Owed(client) == 0 && client.Known.empty() && client.Applied == 0;
			if (joining || adrift) {
				BeginSnapshot(client, store, tick);
			} else if (Owed(client) == 0 && !client.Oversize.empty()) {
				// **After the two above and never beside them.** A join or a
				// re-snapshot carries these entities in the world blob, so
				// staging a slice as well would send the same bytes twice - and
				// the list is cleared by `BeginSnapshot` for exactly that
				// reason. Owed nothing, because a blob part way out must not be
				// replaced by one taken later.
				StageOversize(client, store, tick);
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

			{
				ENGINE_PROFILE_CAT("Authority::Structure", core::ProfileCategory::Network);

				for (const ecs::Entity entity : Visible) {
					if (client.Known.find(entity.Id) == client.Known.end()) {
						structure.Created.push_back(entity);
					}
				}

				{
					// **`Visible` is already ascending by handle**, because the walk
					// above takes `Bearing` in order - so this searches it directly
					// rather than copying every id out and sorting the copy, which
					// was a pass over the client's whole world per client per tick.
					const auto byHandleId = [](const ecs::Entity entity, uint64_t id) {
						return entity.Id < id;
					};

					for (const uint64_t known : client.Known) {
						const auto at = std::lower_bound(Visible.begin(), Visible.end(), known, byHandleId);
						if (at != Visible.end() && at->Id == known) {
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

				if (!structure.Created.empty() || !structure.Destroyed.empty() ||
					!structure.Forgotten.empty()) {
					EmitStructure(client, structure);
				}
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

			{
				ENGINE_PROFILE_CAT("Authority::BuildComponents", core::ProfileCategory::Network);
				BuildComponents(store, client, delta, tick);
			}
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

				// One pass over each set rather than an erase per acknowledged
				// row. An acknowledgement retires everything sent up to a tick,
				// so this is the bulk case by construction.
				for (OutstandingSet &unconfirmed : found->Unconfirmed) {
					const uint64_t applied = found->Applied;
					unconfirmed.EraseIf([applied](const OutstandingSet::Row &row) {
						return row.Value.SentAt != 0 && row.Value.SentAt <= applied;
					});
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
			// world is - the same line a snapshot is refused on.
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
			// one off before here - `Replica::Receive` refuses it for the same
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
		// alternative default - accept, and restrict later - makes the insecure
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
		// an out-of-order one is not a partial update to merge - it is last
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
