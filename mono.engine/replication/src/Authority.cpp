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

		// A component value's identity, for `ChangeDetection::Signature`.
		//
		// **The object representation, and not the form it crosses in.** The
		// wire form is the more obviously correct identity — "send it when what
		// would be sent differs" — and it was the first thing tried. It is the
		// wrong answer here for a reason that only shows up on a profile: a
		// component holding a `core::Name` writes it as *text*, so signing
		// `scene::Visual` that way is two name lookups per entity per tick on a
		// registry shared with every worker in the job system. That is the same
		// lock in the same wrong place that `script/Instances.cpp` documents
		// finding, and it would be paid for the whole world rather than for
		// what changed.
		//
		// **The bytes are a faithful identity here, and two tests are what make
		// that true rather than hoped.** `engine.scene.components` asserts every
		// component is trivially copyable and that none carries unnamed padding
		// — so there are no uninitialised bytes to make one value hash two ways,
		// and no pointer whose target could change behind a stable address.
		// `Resign` refuses to sign a non-trivial type rather than assuming it.
		//
		// **The consequence, stated rather than hidden**: a change too small to
		// survive a quantised wire form still counts as a change, so a
		// `Signature` component with a compact form may resend bytes the client
		// already has. That is the wrong detector for such a component, which is
		// what `ChangeDetection::Signature`'s own comment says.
		//
		// FNV-1a. Not for its distribution — a collision means one update is
		// missed and stays missed — but because it is a byte-at-a-time loop over
		// a few dozen bytes with no table and no allocation, which is what runs
		// once per replicated value per tick.
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

		// What a message costs before any payload: the kind, the version, the
		// tick, the baseline and four counts. Deliberately generous — the
		// consequence of underestimating is a message that does not fit, which
		// is the failure the splitting exists to prevent.
		constexpr size_t MESSAGE_OVERHEAD = 64;

		// A component name is written per entry, so an entry is only worth
		// opening if something can follow it.
		constexpr size_t ENTRY_OVERHEAD = 96;

		// Bytes one value of a component occupies on the wire.
		//
		// **A component may carry a second, compact serialisation, and this and
		// `WriteValue` are the only two places that choose between them.**
		// `TypeDescriptor::Wire` is lossy — a quantised position, a rotation as
		// smallest-three — and it exists as a *second* pair rather than as a
		// codec over `TypeDescriptor::Write` so that `Store::Save` does not
		// become lossy with it. That would make every recording lossy and leave
		// `just replay-check` comparing one lossy file against another, which
		// is a check that passes and means nothing.
		//
		// @param descriptor The component's registration.
		// @return The compact size when it has one, and `sizeof` otherwise.
		size_t WireBytes(const ecs::TypeDescriptor &descriptor) {
			return descriptor.Wire.Present() ? descriptor.Wire.Size : descriptor.Size;
		}

		// Appends one value in whatever form it crosses in.
		//
		// @param writer     Where the bytes go.
		// @param descriptor The component's registration.
		// @param value      The value to write.
		void WriteValue(core::ByteWriter &writer, const ecs::TypeDescriptor &descriptor, const void *value) {
			if (descriptor.Wire.Present()) {
				descriptor.Wire.Write(writer, value, 1);
				return;
			}
			descriptor.Write(writer, value, 1);
		}

		// The largest `ChunkBytes` that still produces a message which fits.
		//
		// **The number to compare against is the plaintext limit, not the
		// payload limit**: every message this builds is sealed by `Session`
		// before it goes, so the Poly1305 tag is sixteen bytes of the datagram
		// that a chunk does not get to spend.
		//
		// Less the message overhead on top of that, because the two readers of
		// `ChunkBytes` read it differently: `Pack` and `EmitStructure` treat it
		// as the whole message budget, and the snapshot streamer treats it as
		// the payload with the message's own fields still to be added. Taking
		// the overhead off covers both, and the overhead is deliberately
		// generous for the same reason it always was.
		constexpr size_t LARGEST_CHUNK = net::Packet::MAXIMUM_MESSAGE_BYTES - MESSAGE_OVERHEAD;
	}

	Authority::Authority(const AuthoritySettings &settings) : Settings_(settings) {
		if (Settings_.ChunkBytes > LARGEST_CHUNK) {
			// **Clamped rather than left to be refused later, and this is the
			// one place in this module where that is the right answer.** A
			// message over the limit is refused by `Link::Reserve`, and a
			// refusal is what ordinary backpressure looks like — so a chunk size
			// that can never fit produces a client that joins and then watches a
			// frozen world, with `SendsOverBudget` as the only clue and "the
			// link is busy" as the reading everybody takes from it. That exact
			// bug has been found here three times: the v0.3 delta, the snapshot
			// chunk cursor and the oversized forget.
			ENGINE_WARN(
				"replication: a chunk of {} bytes cannot fit a sealed datagram, so it is capped at {}.",
				Settings_.ChunkBytes,
				LARGEST_CHUNK
			);
			Settings_.ChunkBytes = LARGEST_CHUNK;
		}

		if (Settings_.MessagesPerTick > MAXIMUM_PARTS) {
			// **A part a receiver refuses is a part that never arrives**, and a
			// tick missing a part is a tick the client will not acknowledge —
			// so a limit above what `Delta::Part` may carry would stall the
			// stream rather than merely waste the excess. Capped here for the
			// same reason `ChunkBytes` is: the alternative is a setting that
			// looks accepted and produces a client which joins and then watches
			// a frozen world.
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
			// **The later call wins rather than being ignored.** A caller
			// naming a component twice with two detectors has changed its mind,
			// and silently keeping the first would be a component that does not
			// replicate for a reason nothing reports.
			Detection[static_cast<size_t>(std::distance(Components.begin(), at))] = detection;
			return;
		}

		Components.push_back(component);
		Detection.push_back(detection);
		Signatures.emplace_back();
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

		Resign(store);
	}

	void Authority::Resign(ecs::Store &store) {
		// **Once per `Publish`, not once per client.** Whether a value moved
		// has one answer for the whole server; which client has *received* it
		// is `Client::Unconfirmed`'s separate question.
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
				// A tag has no value to hash and therefore nothing that can
				// change while it is present. Its arrival and departure are
				// structural and are already said by `Created` and `Destroyed`.
				continue;
			}

			if (!descriptor.Trivial) {
				// **The identity below is the object representation**, so a
				// type that owns anything would be hashing a pointer — stable
				// while the value behind it changed, which is a component that
				// silently stops replicating. Refused rather than approximated.
				ENGINE_WARN(
					"replication: '{}' is not trivially copyable and cannot be signed; "
					"observe it instead.",
					Components[slot].Text()
				);
				continue;
			}

			// **Walked in `Bearing` order**, which is sorted — so `Changed`
			// comes out sorted without a second pass, and two runs of one
			// server produce the same bytes.
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
					// First sight. Offered rather than assumed known: the row
					// may have been created this tick, and a client that
					// already knows the entity is owed the value.
					signature.Changed.push_back(id64);
					continue;
				}
				if (entry->second != hash) {
					entry->second = hash;
					signature.Changed.push_back(id64);
				}
			}

			// **Swept only when the count disagrees.** A steady world walks
			// this map once per tick and never erases from it; paying for a
			// second pass every tick to find nothing would be the cost this
			// detector is already accused of.
			if (signature.Hashes.size() == present) {
				continue;
			}

			for (auto entry = signature.Hashes.begin(); entry != signature.Hashes.end();) {
				const ecs::Entity entity{entry->first};
				if (!store.Alive(entity) || store.GetComponent(entity, id) == nullptr) {
					// Dropped, so the component coming back later reads as a
					// change rather than as the value it had when it left.
					entry = signature.Hashes.erase(entry);
					continue;
				}
				++entry;
			}
		}
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

		// **The snapshot carries what a delta would have carried, and that is a
		// decision rather than an accident of where the copy happens.** A join
		// snapshot is built from this scratch world and a delta is built from
		// the dirty bits, so a component with a compact wire form would reach a
		// client at full precision through one path and quantised through the
		// other — and the same entity would hold a different value depending on
		// how that client learned it. That difference never shows as a failure;
		// it shows as drift between two clients that joined at different
		// moments. So every value with a wire form is put *through* it here,
		// and the snapshot writes the value the far side would have decoded.
		//
		// The scratch world is the only place this is safe. Round-tripping the
		// authoritative store would be quantising the server's own simulation,
		// which is what `just determinism` exists to catch.
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

			for (const ecs::Entity entity : Visible) {
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
		// under `ChunkBytes` and **each independently applicable**. Still not a
		// reassembly: this is the unreliable channel, and holding a part back
		// until its siblings arrive is a stall on a path whose whole premise is
		// that the next tick is already on its way. Every part is applied the
		// moment it lands.
		//
		// **What the parts are numbered for is the acknowledgement, not the
		// application.** `Applied` names the last tick applied *in full*, and
		// the server retires every value a tick carried once the client says so
		// — so a client that acknowledged a tick it received five parts of six
		// of retired the sixth part's values unsent, and anything in it that
		// then stopped moving was wrong until a re-snapshot nothing would ask
		// for. D00013. The numbering below is what lets the client tell the two
		// apart; the marker is set once the packing has finished, at the bottom
		// of this function.
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

		// The last part that actually went, and where it sits in `Outgoing`, so
		// the final marker can be put on it once it is known to be the last.
		Delta emitted;
		size_t emittedAt = NOWHERE;

		// Sends the message being built, unless doing so would spend more than
		// this client's tick is worth. Everything after a refusal stays
		// unbuilt, which is what makes `Placement` a prefix.
		const auto flush = [&]() -> bool {
			if (!piece.Components.empty()) {
				// Numbered by how many parts of this tick have actually gone,
				// so the numbers a receiver sees are 0, 1, 2 with no holes —
				// a part counted before the budget refused it would be a hole
				// the receiver waits on for a message that was never built.
				piece.Part = static_cast<uint16_t>(messages);
				piece.Final = false;

				std::vector<std::byte> encoded = Encode(piece);
				if (messages >= messageLimit || bytes + encoded.size() > Settings_.BytesPerTick) {
					return false;
				}

				bytes += encoded.size();
				messages++;
				placed.Values += rows;

				// **Nothing to undo, so nothing is recorded.** A delta moves no
				// entity in or out of the known set — that is `EmitStructure`'s
				// — and a component value in a refused delta is offered again
				// next tick by the unconfirmed set.
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

		// **"All of tick N" is the parts this pass emitted, and nothing else.**
		// Written here rather than while a part is being built because until
		// the packing has finished nothing knows which part was the last — the
		// loop above stops on a full message, a full budget or a full tick, and
		// only the third of those is visible from inside `flush`.
		//
		// This is also the whole reason the byte budget cannot make a tick look
		// incomplete. Everything the cap held over was never part of tick N: it
		// keeps its unconfirmed entry, comes back on a later tick, and the
		// client is told this tick ended here. A marker meaning "nothing else
		// changed" would leave every trimmed tick unacknowledged, which is a
		// stalled stream on exactly the servers the cap exists for.
		//
		// Re-encoded rather than patched at an offset. The two fields are fixed
		// width, so the size is unchanged and the byte accounting above stays
		// exact; an offset would be a second place that has to agree with
		// `WriteMessage` about the layout.
		if (emittedAt != NOWHERE) {
			emitted.Final = true;
			client.Outgoing[emittedAt] = Encode(emitted);
		}

		return placed;
	}

	void Authority::EmitStructure(Client &client, const Structure &structure) {
		// **Split for the same reason a delta is.** A world going out of view
		// all at once names every entity in one message, and three hundred
		// handles is tens of kilobytes against a twelve-hundred-byte payload —
		// which `Link::Reserve` refuses outright rather than fragmenting, so the
		// one message that says "stop drawing these" is the one that never
		// arrives.
		//
		// At least one entity per message, whatever `ChunkBytes` was set to. A
		// subtraction that underflowed would put the whole list in one message
		// again, and a zero would never advance the offset.
		const size_t room =
			Settings_.ChunkBytes > MESSAGE_OVERHEAD ? Settings_.ChunkBytes - MESSAGE_OVERHEAD : 0;
		const size_t perMessage = std::max<size_t>(1, room / sizeof(uint64_t));

		// Created, then destroyed, then forgotten, each in its own messages.
		// Creations lead because a component value for a row the receiver has
		// not made yet is dropped, and the delta follows all of these.
		const std::array<const std::vector<ecs::Entity> *, 3> lists{
			&structure.Created, &structure.Destroyed, &structure.Forgotten
		};

		// Whether undoing the message means putting the entity back in the known
		// set. A creation put one in; a destroy and a forget each took one out.
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

				// What this message moved in the known set, so a refusal can
				// move it back. Each of the three is said exactly once.
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

			// **Every value of an entity the client has just been told about,
			// whether or not it changed this tick.** The dirty bits describe
			// what *moved*, and an entity entering a client's interest has not
			// moved — it was always there and this client could not see it. So
			// the run walk below finds nothing for it and the entity used to
			// come into view as a bare row holding none of its components, for
			// as long as it stood still.
			//
			// Seeding an unconfirmed entry rather than offering it here on
			// purpose: the recovery walk already reads the current value,
			// already skips an entity that has no such component, and already
			// keeps offering until the client confirms. A second path that did
			// the same thing is the second one that would rot.
			for (const uint64_t appearing : Appearing) {
				unconfirmed.emplace(appearing, Outstanding{});
			}

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

			// Against the form it actually crosses in, not against `sizeof`. A
			// component with a compact wire form that fits and a stored size
			// that does not would otherwise be refused for a size nothing sends.
			const size_t crossing = WireBytes(descriptor);
			if (MESSAGE_OVERHEAD + ENTRY_OVERHEAD + sizeof(uint64_t) + crossing > Settings_.ChunkBytes) {
				// One entity of this component does not fit an empty message.
				// Nothing can be done about it here and dropping it quietly
				// would be a component that never arrives.
				ENGINE_WARN(
					"replication: '{}' is {} bytes on the wire and cannot fit a delta message.",
					name.Text(),
					crossing
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

			core::ByteWriter values;

			if (Detection[slot] == ChangeDetection::Signature) {
				// **What `Resign` found, which is the same list for every
				// client** — it was computed once per `Publish` from the values
				// themselves rather than per client from the dirty bits.
				//
				// Read one at a time through `GetComponent`, as the recovery
				// walk below does and for the same reason: what is wanted is
				// the value now, and this list names entities rather than runs.
				// There is no run to memcpy — the whole point of this detector
				// is that it serves components which change a few rows at a
				// time and not a column at a time.
				for (const uint64_t changed : Signatures[slot].Changed) {
					const ecs::Entity entity{changed};
					if (client.Known.find(changed) == client.Known.end()) {
						continue;
					}

					const void *value = store.GetComponent(entity, id);
					if (value == nullptr) {
						continue;
					}

					offer(entity);
					WriteValue(values, descriptor, value);
				}
			} else {
				// Runs, not rows. `EachChangedBatch` hands over adjacent changed
				// rows as a block precisely so a delta is a memcpy per run rather
				// than a copy per entity.
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
							WriteValue(
								values,
								descriptor,
								static_cast<const std::byte *>(data) + row * descriptor.Size
							);
						}
					}
				});
			}

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
					WriteValue(values, descriptor, value);
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

	void Authority::Record(Client &client, const Placement &placed, uint64_t tick) {
		// **Values only, and that is a change rather than an omission.** This is
		// what the re-snapshot decision is measured against, and a client
		// acknowledges the last tick whose *state* it applied — a structural
		// message carries none and `Replica` deliberately does not move
		// `Applied` for it. Counting a structure-only tick as streamed would
		// leave `Streamed` permanently ahead of `Applied` in a world where
		// entities come and go and nothing moves, and re-snapshot a client that
		// is in perfect agreement. The same argument as the quiet world above.
		if (placed.Values > 0) {
			// Kept so `Unsent` can put it back. A tick whose parts the
			// transport would not all take is a tick the client cannot
			// acknowledge — every part of it has to arrive before `Applied` may
			// name it — so it must not be the tick the client's silence is
			// measured against.
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

			// --- the structure ---
			//
			// Which entities the client holds, in one reliable message rather
			// than riding the delta. **A creation is said exactly once and the
			// known set moves when it is said**, so a lost one leaves the server
			// certain the client was told about something it has never heard of
			// — and nothing above notices, because the client goes on
			// acknowledging ticks. D00011.
			Structure structure;
			structure.Tick = tick;

			// Created: visible now, not known before. Their values follow in the
			// delta, because the known set moves below and `BuildComponents`
			// filters by exactly it.
			for (const ecs::Entity entity : Visible) {
				if (client.Known.find(entity.Id) == client.Known.end()) {
					structure.Created.push_back(entity);
				}
			}

			// Destroyed and forgotten, told apart deliberately. An entity that
			// went out of view still exists, and a client that deleted it would
			// be wrong about the world the moment it came back.
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

			// Deterministic, because two runs of one server must produce the
			// same bytes — the same rule a snapshot follows, for the same
			// reason. An unordered set is walked in whatever order it likes.
			const auto byHandle = [](ecs::Entity left, ecs::Entity right) { return left.Id < right.Id; };
			std::sort(structure.Created.begin(), structure.Created.end(), byHandle);
			std::sort(structure.Destroyed.begin(), structure.Destroyed.end(), byHandle);
			std::sort(structure.Forgotten.begin(), structure.Forgotten.end(), byHandle);

			// The known set moves before the delta is built, because a
			// creation's component values belong in the same tick as the
			// creation and `BuildComponents` filters by exactly this set.
			// `Unsent` puts back whatever the transport then would not take.
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

			// What `BuildComponents` owes a full value rather than a delta. See
			// the note there.
			Appearing.clear();
			for (const ecs::Entity entity : structure.Created) {
				Appearing.push_back(entity.Id);
			}

			// --- the delta ---
			//
			// Everything before this belongs to the client's known set and must
			// survive the re-pack below, so the structure's messages are counted
			// and the repack rewinds only to here.
			const size_t structureMessages = client.Outgoing.size();
			const size_t structureEdits = client.Edits.size();

			Delta delta;
			delta.Tick = tick;
			delta.Baseline = client.Applied;

			BuildComponents(store, client, delta, tick);

			if (!delta.Components.empty()) {
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

				if (placed.Values < Candidates.size()) {
					// It did not fit, so *which* of it goes is a decision now
					// rather than an accident of where a component sits in a
					// vector. D00007. The structure's messages stay: they are
					// not ranked, and re-encoding them would mint a second copy
					// of an edit the known set has already moved for.
					client.Outgoing.resize(structureMessages);
					client.Carried_.resize(structureMessages);
					client.Edits.resize(structureEdits);

					Prioritise(handle, tick);
					placed = Pack(client, delta, Settings_.MessagesPerTick);
				}

				Record(client, placed, tick);
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

		if (carried.Values) {
			// **The values still need nothing undone; the tick does.** They are
			// offered again next tick by the unconfirmed set and they are not
			// retired in the meantime, because a client only acknowledges a tick
			// it holds every part of — and a part the transport would not take
			// is a part that never arrives, so this tick will never be
			// acknowledged.
			//
			// Which is exactly why it must stop counting as a tick that
			// streamed. Left alone, a link whose packet budget is below
			// `MessagesPerTick` cuts every tick short, `Streamed` runs away from
			// an `Applied` that cannot follow it, and the client is
			// re-snapshotted every `ResnapshotAfterTicks` for ever — the whole
			// world, twice a minute, over the link that was already refusing a
			// delta. `ConnectionStats::SendsOverBudget` is what says this is
			// happening; a re-snapshot loop is not.
			found->Streamed = found->StreamedBefore;
		}

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
		case MessageKind::Structure:
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
