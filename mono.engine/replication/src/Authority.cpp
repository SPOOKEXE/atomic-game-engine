#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/replication/Authority.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

		// What a message costs before any payload: the kind, the version, the
		// tick, the baseline and four counts. Deliberately generous — the
		// consequence of underestimating is a message that does not fit, which
		// is the failure the splitting exists to prevent.
		constexpr size_t MESSAGE_OVERHEAD = 64;

		// A component name is written per entry, so an entry is only worth
		// opening if something can follow it.
		constexpr size_t ENTRY_OVERHEAD = 96;
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

	void Authority::SetPriority(std::function<float(ClientId, ecs::Entity)> score) {
		Priority = std::move(score);
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

	void Authority::Survey(ecs::Store &store) {
		// The replicated components, resolved once. `Components::Find` is a
		// name lookup and this walk asks about every entity, so resolving
		// inside the walk would be one lookup per entity per component.
		Resolved.clear();
		for (const core::Name name : Components) {
			if (const ecs::ComponentId id = ecs::Components::Find(name); id.IsValid()) {
				Resolved.push_back(id);
			}
		}

		// **Which entities have anything to send.** An entity with no
		// replicated component is not something a client is told nothing about
		// — it is something a client is not told about at all, because the row
		// it would appear as in a join snapshot is a count of the world leaking
		// past both filters this module has. Interest filters entities and
		// `Replicate` filters components, and neither one caught the entity
		// that passed the first and had nothing left after the second.
		//
		// Once per `Publish` rather than once per client: the answer is the
		// same for everybody.
		Bearing.clear();
		store.EachEntity([this, &store](ecs::Entity entity) {
			for (const ecs::ComponentId id : Resolved) {
				if (store.HasComponent(entity, id)) {
					Bearing.push_back(entity.Id);
					return;
				}
			}
		});

		// Sorted so the per-client filter is a binary search rather than a
		// scan, and so two runs of one server ask in the same order.
		std::sort(Bearing.begin(), Bearing.end());
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
		for (std::unordered_map<uint64_t, Outstanding> &unconfirmed : client.Unconfirmed) {
			unconfirmed.clear();
		}
	}

	void Authority::Prioritise(ClientId client, uint64_t tick) {
		// The score, once per entity. Asked for here rather than while the
		// values were being copied, because on a tick that fits nobody asks at
		// all — and a hook called sixty times a second per entity per client
		// for an answer nothing reads is the kind of cost that never shows up
		// as a line in a profile.
		if (Priority) {
			for (Candidate &candidate : Candidates) {
				const float hint = Priority(client, candidate.Entity);

				// A NaN is not a weak ordering. `std::sort` on a comparator
				// that is not one is undefined rather than merely surprising,
				// and this is a value a game supplies.
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

			// **The rotation outranks the score rather than being added to
			// it.** A weighted sum lets a permanently high score hold a low
			// one off the wire for the life of the connection, and D00007 is
			// explicit that the visible symptom of that is a component looking
			// broken rather than a budget looking small. A value that has
			// waited its deadline out jumps every score there is, so the
			// longest anything waits is the deadline plus the ticks it takes
			// to drain the queue of things that waited longer.
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

			// **A total order, not a tie-break of convenience.** `std::sort`
			// is not stable, so two candidates that compared equal would come
			// out in whatever order the implementation liked and two runs of
			// one server would disagree about the bytes on the wire.
			if (first.Entity.Id != second.Entity.Id) {
				return first.Entity.Id < second.Entity.Id;
			}
			return first.Entry < second.Entry;
		});
	}

	Authority::Placement Authority::Pack(Client &client, const Delta &delta, size_t messageLimit) {
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
		//
		// **And however many it takes is not however many the link will
		// carry.** Past `messageLimit` the rest is held over to a later tick,
		// in `Order`'s order — which is the decision D00007 says nobody had
		// made. What is held over keeps its place in the unconfirmed set and
		// comes back with a longer wait behind it.
		const size_t budget = Settings_.ChunkBytes;

		Placement placed;
		size_t messages = 0;
		size_t bytes = 0;
		size_t used = MESSAGE_OVERHEAD;
		size_t rows = 0;

		Delta piece;
		piece.Tick = delta.Tick;
		piece.Baseline = delta.Baseline;
		OpenEntry.assign(delta.Components.size(), NOWHERE);

		// Sends the message being built, unless doing so would spend more than
		// this client's tick is worth. Everything after a refusal stays
		// unbuilt, which is what makes `Placement` a prefix.
		const auto flush = [&]() -> bool {
			if (!piece.Created.empty() || !piece.Destroyed.empty() || !piece.Components.empty()) {
				std::vector<std::byte> encoded = Encode(piece);
				if (messages >= messageLimit || bytes + encoded.size() > Settings_.BytesPerTick) {
					return false;
				}

				bytes += encoded.size();
				messages++;
				placed.Created += piece.Created.size();
				placed.Destroyed += piece.Destroyed.size();
				placed.Values += rows;

				// What this message moved in the known set, so a refusal can
				// move it back. A creation put an entity in and a destruction
				// took one out, and either one is said exactly once.
				Carried carried;
				carried.First = static_cast<uint32_t>(client.Edits.size());
				carried.Count = static_cast<uint32_t>(piece.Created.size() + piece.Destroyed.size());
				for (const ecs::Entity entity : piece.Created) {
					client.Edits.push_back(Edit{entity.Id, false});
				}
				for (const ecs::Entity entity : piece.Destroyed) {
					client.Edits.push_back(Edit{entity.Id, true});
				}

				client.Outgoing.push_back(std::move(encoded));
				client.Carried_.push_back(carried);
			}

			piece = Delta{};
			piece.Tick = delta.Tick;
			piece.Baseline = delta.Baseline;
			used = MESSAGE_OVERHEAD;
			rows = 0;
			OpenEntry.assign(delta.Components.size(), NOWHERE);
			return true;
		};

		// Structure first, and in its own messages when it needs them. A
		// component value for a row the receiver has not created yet is
		// dropped, so creations lead.
		//
		// **Structure is not rotated, and does not need to be.** An entity is
		// created once and destroyed once, so a creation this tick's budget
		// could not carry is carried by the next one and cannot recur — where
		// a component value changes every tick and starves for good if it is
		// always last.
		bool room = true;
		const auto place = [&](std::vector<ecs::Entity> &into, const std::vector<ecs::Entity> &from) {
			for (const ecs::Entity entity : from) {
				if (used + sizeof(uint64_t) > budget && !(room = flush())) {
					return;
				}
				into.push_back(entity);
				used += sizeof(uint64_t);
			}
		};

		place(piece.Created, delta.Created);
		if (room) {
			place(piece.Destroyed, delta.Destroyed);
		}

		for (size_t position = 0; room && position < Order.size(); position++) {
			const Candidate &candidate = Candidates[Order[position]];
			const size_t stride = Strides[candidate.Entry];
			const size_t perEntity = sizeof(uint64_t) + stride;

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
				source.Values.begin() + static_cast<ptrdiff_t>(candidate.Row * stride),
				source.Values.begin() + static_cast<ptrdiff_t>((candidate.Row + 1) * stride)
			);

			used += needs;
			rows++;
		}

		if (room) {
			flush();
		}
		return placed;
	}

	void Authority::EmitForget(Client &client, const Forget &forget) {
		// **Split for the same reason a delta is.** A forget naming every
		// entity of a world that went out of view is tens of kilobytes against
		// a twelve-hundred-byte payload, and `Link::Reserve` refuses an
		// oversized message rather than fragmenting it — so the one message
		// that says "stop drawing these" is the one that never arrives, and
		// the client draws them for the rest of the connection.
		// At least one, whatever `ChunkBytes` was set to. A subtraction that
		// underflowed would put the whole list in one message again, and a zero
		// would never advance the offset.
		const size_t room =
			Settings_.ChunkBytes > MESSAGE_OVERHEAD ? Settings_.ChunkBytes - MESSAGE_OVERHEAD : 0;
		const size_t perMessage = std::max<size_t>(1, room / sizeof(uint64_t));

		for (size_t offset = 0; offset < forget.Entities.size(); offset += perMessage) {
			const size_t take = std::min(perMessage, forget.Entities.size() - offset);

			Forget piece;
			piece.Tick = forget.Tick;
			piece.Entities.assign(
				forget.Entities.begin() + static_cast<ptrdiff_t>(offset),
				forget.Entities.begin() + static_cast<ptrdiff_t>(offset + take)
			);

			Carried carried;
			carried.First = static_cast<uint32_t>(client.Edits.size());
			carried.Count = static_cast<uint32_t>(take);
			for (const ecs::Entity entity : piece.Entities) {
				client.Edits.push_back(Edit{entity.Id, true});
			}

			client.Outgoing.push_back(Encode(piece));
			client.Carried_.push_back(carried);
		}
	}

	void Authority::BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick) {
		// One map per replicated component, and the list only grows, so an
		// index into it is stable for the life of the server.
		client.Unconfirmed.resize(Components.size());

		Candidates.clear();
		Strides.clear();
		SourceSlot.clear();

		for (size_t slot = 0; slot < Components.size(); slot++) {
			const core::Name name = Components[slot];
			std::unordered_map<uint64_t, Outstanding> &unconfirmed = client.Unconfirmed[slot];

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

			if (MESSAGE_OVERHEAD + ENTRY_OVERHEAD + sizeof(uint64_t) + descriptor.Size >
				Settings_.ChunkBytes) {
				// One entity of this component does not fit an empty message.
				// Nothing can be done about it here and dropping it quietly
				// would be a component that never arrives.
				ENGINE_WARN(
					"replication: '{}' is {} bytes and cannot fit a delta message.",
					name.Text(),
					descriptor.Size
				);
				continue;
			}

			ComponentDelta component;
			component.Component = name;

			// Where this component's entry will sit once it is known to carry
			// anything. Recorded before the walk so a candidate can name it.
			const uint32_t entry = static_cast<uint32_t>(delta.Components.size());
			const size_t before = Candidates.size();

			// The value is copied and the wait is noted; **nothing is stamped
			// as sent here.** What goes out this tick is not known until the
			// budget has had its say, and a value stamped with a tick it never
			// left on is one an acknowledgement will retire without it ever
			// having arrived.
			const auto offer = [&](ecs::Entity entity) {
				Outstanding &pending = unconfirmed[entity.Id];
				if (pending.WaitingSince == 0) {
					pending.WaitingSince = tick;
				}
				pending.ConsideredAt = tick;

				Candidates.push_back(
					Candidate{
						entry,
						static_cast<uint32_t>(component.Entities.size()),
						entity,
						pending.WaitingSince,
						0.0f
					}
				);
				component.Entities.push_back(entity);
			};

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

					offer(entity);
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
			//
			// **Collected and sorted rather than walked in place.** An
			// unordered map is iterated in whatever order it likes, so the same
			// world produced a different byte order in two runs of one server —
			// which nothing caught, because a recorded run serves no clients.
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
					// Gone or no longer visible. `Destroyed` and `Forget` say so
					// on their own; holding a resend for it would be resending a
					// value for a row the client has been told to drop.
					unconfirmed.erase(known);
					continue;
				}

				const void *value = store.GetComponent(entity, id);
				if (value == nullptr) {
					unconfirmed.erase(known);
					continue;
				}

				offer(entity);
				if (descriptor.Size > 0) {
					descriptor.Write(values, static_cast<const std::byte *>(value), 1);
				}
			}

			if (component.Entities.empty()) {
				Candidates.resize(before);
				continue;
			}

			component.Values.assign(values.Bytes().begin(), values.Bytes().end());

			// From the entry itself rather than from the descriptor, because
			// this is the number of bytes that were actually written for each
			// entity — a type with a custom serialiser is not its `sizeof`.
			Strides.push_back(component.Values.size() / component.Entities.size());
			SourceSlot.push_back(slot);
			delta.Components.push_back(std::move(component));
		}
	}

	void Authority::Record(Client &client, const Delta &delta, const Placement &placed, uint64_t tick) {
		if (placed.Created + placed.Destroyed + placed.Values > 0) {
			// The tick something last went out on, which is what the
			// re-snapshot decision is measured against — see `Publish`.
			client.Streamed = tick;
		}

		// Known only for the creations that actually went out. An entity marked
		// known but never announced is one whose component values are then sent
		// for a row the client does not hold — and `Replica` drops those,
		// silently, for as long as the entity lives.
		for (size_t index = placed.Created; index < delta.Created.size(); index++) {
			client.Known.erase(delta.Created[index].Id);
		}
		for (size_t index = placed.Destroyed; index < delta.Destroyed.size(); index++) {
			client.Known.insert(delta.Destroyed[index].Id);
		}

		for (size_t position = 0; position < Order.size(); position++) {
			const Candidate &candidate = Candidates[Order[position]];
			Outstanding &pending = client.Unconfirmed[SourceSlot[candidate.Entry]][candidate.Entity.Id];

			if (position < placed.Values) {
				pending.SentAt = tick;
				pending.WaitingSince = 0;
				continue;
			}

			// Held over by the cap. **Zero, not the tick it last went out
			// on**: an acknowledgement retires an entry whose tick it can see,
			// and retiring this one would confirm a value that never left. The
			// wait keeps running, which is what puts it in front next time.
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

		if (Count() == 0) {
			// Nobody to tell, so nothing to work out. A server ticks whether or
			// not anybody is connected, and `Survey` walks the world.
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

			// Released a tick after the cursor reached the end rather than at
			// the moment it did. `Unsent` rewinds that cursor for a chunk the
			// transport would not take and is called after `Publish` has
			// returned, so freeing here would free the only copy of a chunk
			// that still has to go — and a snapshot with a hole in it is a
			// client that streams almost all of a world and then never joins.
			if (!client.Snapshot.empty() && client.Sent >= client.Snapshot.size()) {
				client.Snapshot.clear();
				client.Snapshot.shrink_to_fit();
				client.Sent = 0;
			}

			// What this client may see, this tick. Recomputed rather than
			// remembered: interest depends on the world, and a cached answer is
			// wrong exactly when something moved.
			Visible.clear();
			store.EachEntity([this, handle](ecs::Entity entity) {
				// Nothing to send about it, so nothing is said about it — not
				// even the empty row a join snapshot would otherwise carry.
				// See `Bearing`.
				if (!std::binary_search(Bearing.begin(), Bearing.end(), entity.Id)) {
					return;
				}
				if (!Interest || Interest(handle, entity)) {
					Visible.push_back(entity);
				}
			});
			Stats_.Visible += Visible.size();

			// A client this far behind cannot be caught up by deltas it never
			// received. Restarting it is the honest answer and the one that
			// needs no repair path nobody tests.
			//
			// **Measured against what was sent, not against the tick number.** A
			// client acknowledges the last tick it *applied*, and a world where
			// nothing moves sends no delta for it to apply — so a client in
			// perfect agreement with a quiet world stopped acknowledging new
			// ticks and was re-snapshotted for it, every hundred and twenty-one
			// ticks, for as long as the world stayed quiet. The whole world,
			// twice a minute, to repair a client that was already correct.
			const bool adrift = client.Snapshot.empty() && client.Sent == 0 && client.Applied > 0 &&
								client.Streamed > client.Applied &&
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

					// The offset travels with the message so that a refusal can
					// put the cursor back. **This is what a chunk needs and a
					// delta does not**: the delta is rebuilt from the
					// unconfirmed set next tick, and the snapshot has no such
					// record — the cursor moving is the only memory of it.
					Carried carried;
					carried.SnapshotOffset = client.Sent;

					client.Outgoing.push_back(Encode(piece));
					client.Carried_.push_back(carried);
					client.Sent += take;
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

			// The known set moves before the delta is built, because a
			// creation's component values belong in the same delta as the
			// creation and `BuildComponents` filters by exactly this set.
			// `Record` puts back whatever the budget then could not carry.
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
				// **Natural order first: component by component and run by
				// run, exactly as the dirty bits handed them over.** When the
				// whole tick fits, that is the end of it — the priority pass
				// below never runs, nothing is reordered and not one byte is
				// spent on deciding. A scheme that ranked every entity whether
				// or not it needed to would be a tax on every server that was
				// never over budget.
				Order.resize(Candidates.size());
				for (size_t position = 0; position < Order.size(); position++) {
					Order[position] = static_cast<uint32_t>(position);
				}

				Placement placed = Pack(client, delta, Settings_.MessagesPerTick);
				const bool overBudget = placed.Created < delta.Created.size() ||
										placed.Destroyed < delta.Destroyed.size() ||
										placed.Values < Candidates.size();

				if (overBudget) {
					// It did not fit, so *which* of it goes is a decision now
					// rather than an accident of where a component sits in a
					// vector. D00007.
					client.Outgoing.clear();
					client.Carried_.clear();
					client.Edits.clear();

					Prioritise(handle, tick);
					placed = Pack(client, delta, Settings_.MessagesPerTick);
				}

				Record(client, delta, placed, tick);
			}

			if (!forget.Entities.empty()) {
				EmitForget(client, forget);
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

	void Authority::Unsent(ClientId client, size_t index) {
		Client *found = Reach(client);
		if (found == nullptr || index >= found->Carried_.size()) {
			return;
		}

		const Carried &carried = found->Carried_[index];

		if (carried.SnapshotOffset != NOWHERE) {
			// **Back to the earliest byte that did not go**, not to this
			// chunk's own offset alone: the transport refuses the tail of a
			// tick together, and rewinding to the first of them re-sends
			// exactly those. A duplicate chunk is free — `Replica` tracks which
			// bytes it already has — and a missing one is a client that never
			// joins.
			found->Sent = std::min(found->Sent, carried.SnapshotOffset);
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
			// Never backwards. A client claiming to have applied a tick older
			// than one it already confirmed is either reordered or lying, and
			// believing it would restart a stream that is working.
			if (read.Applied.Tick > found->Applied) {
				found->Applied = read.Applied.Tick;

				// Retire what this confirms. An entry sent at or before the tick
				// the client says it applied has arrived, so it stops being
				// resent — and an entry that has not yet gone out at all, which
				// is the zero, is not confirmed by anything.
				for (std::unordered_map<uint64_t, Outstanding> &unconfirmed : found->Unconfirmed) {
					for (auto entry = unconfirmed.begin(); entry != unconfirmed.end();) {
						const uint64_t sentAt = entry->second.SentAt;
						const bool confirmed = sentAt != 0 && sentAt <= found->Applied;
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
