#include "BusRouter.hpp"

#include <engine/core/Log.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace engine::world {

	World *WorldDirectory::Reach(WorldId id) const {
		if (!id.IsValid() || id.Index >= Registry.size()) {
			return nullptr;
		}
		return Registry[id.Index].get();
	}

	WorldId WorldDirectory::Find(core::Name name) const {
		for (size_t index = 0; index < Registry.size(); index++) {
			if (Registry[index] != nullptr && Registry[index]->Name() == name) {
				return WorldId{static_cast<uint32_t>(index)};
			}
		}
		return WorldId{};
	}

	core::Name WorldDirectory::HostOf(WorldId id) const {
		if (Reach(id) == nullptr || id.Index >= Hosts.size()) {
			return {};
		}
		return Hosts[id.Index];
	}

	bool BusRouter::Deliver(WorldId id, Delivery delivery) {
		if (!id.IsValid() || id.Index >= Fanout.size()) {
			return false;
		}
		Fanout[id.Index].push_back(std::move(delivery));
		return true;
	}

	void BusRouter::ApplyEnvelope(World &sender, const Envelope &envelope, const WorldDirectory &directory) {
		const uint32_t key = envelope.Key.Id();

		// The reply this operation produces, if it asked for one. Built here
		// and posted at the end, so every path fills the same record.
		Delivery reply;
		reply.Bus = envelope.Bus;
		reply.Key = envelope.Key;
		reply.Reply = envelope.Reply;
		reply.Status = BusStatus::Ok;

		switch (envelope.Bus) {
		case BusKind::Messaging: {
			auto &topics = Backends.Messaging.Subscribers;

			if (envelope.Operation == BusOperation::Subscribe) {
				topics[key].insert(sender.Id().Index);
				break;
			}
			if (envelope.Operation == BusOperation::Unsubscribe) {
				const auto found = topics.find(key);
				if (found != topics.end()) {
					found->second.erase(sender.Id().Index);
				}
				break;
			}
			if (envelope.Operation != BusOperation::Publish) {
				reply.Status = BusStatus::Unsupported;
				break;
			}

			const auto found = topics.find(key);
			if (found == topics.end()) {
				// A publish with no subscribers is a quiet afternoon, not
				// an error.
				break;
			}

			for (const uint32_t listener : found->second) {
				if (listener == sender.Id().Index) {
					// Not delivered back to the publisher. A world that had
					// to filter its own messages out of its own inbox would
					// get it wrong exactly once.
					continue;
				}

				Delivery message;
				message.Bus = BusKind::Messaging;
				message.Key = envelope.Key;
				message.From = sender.Name();
				message.Payload = envelope.Payload;
				Deliver(WorldId{listener}, std::move(message));
			}
			break;
		}

		case BusKind::MemoryStore: {
			auto &values = Backends.Memory.Values;
			auto &queues = Backends.Memory.Queues;

			switch (envelope.Operation) {
			case BusOperation::Get: {
				const auto found = values.find(key);
				if (found == values.end()) {
					reply.Status = BusStatus::NotFound;
				} else {
					reply.Payload = found->second;
				}
				break;
			}
			case BusOperation::Set:
				values[key] = envelope.Payload;
				break;
			case BusOperation::Remove:
				if (values.erase(key) == 0) {
					reply.Status = BusStatus::NotFound;
				}
				break;
			case BusOperation::Push:
				queues[key].push_back(envelope.Payload);
				break;
			case BusOperation::Pop: {
				const auto found = queues.find(key);
				if (found == queues.end() || found->second.empty()) {
					reply.Status = BusStatus::NotFound;
				} else {
					// One at a time, in barrier order. Several worlds
					// popping the same queue each get a different entry
					// because the barrier applies them one after another
					// rather than concurrently.
					reply.Payload = std::move(found->second.front());
					found->second.pop_front();
				}
				break;
			}
			default:
				reply.Status = BusStatus::Unsupported;
				break;
			}
			break;
		}

		case BusKind::DataStore: {
			auto &records = Backends.Data.Records;

			switch (envelope.Operation) {
			case BusOperation::Get: {
				const auto found = records.find(key);
				if (found == records.end()) {
					reply.Status = BusStatus::NotFound;
				} else {
					reply.Payload = found->second.Value;
					reply.Version = found->second.Version;
				}
				break;
			}
			case BusOperation::Set: {
				DataBus::Record &record = records[key];
				record.Value = envelope.Payload;
				record.Version++;
				reply.Version = record.Version;
				break;
			}
			case BusOperation::Update: {
				const auto found = records.find(key);
				const uint64_t current = found == records.end() ? 0 : found->second.Version;

				if (current != envelope.Version) {
					// The value moved between the caller's read and its
					// write. Refused rather than overwritten, because
					// two worlds updating one player's inventory must
					// not silently lose one of the writes.
					reply.Status = BusStatus::Conflict;
					reply.Version = current;
					if (found != records.end()) {
						reply.Payload = found->second.Value;
					}
					break;
				}

				DataBus::Record &record = records[key];
				record.Value = envelope.Payload;
				record.Version++;
				reply.Version = record.Version;
				break;
			}
			case BusOperation::Remove:
				if (records.erase(key) == 0) {
					reply.Status = BusStatus::NotFound;
				}
				break;
			default:
				reply.Status = BusStatus::Unsupported;
				break;
			}
			break;
		}

		case BusKind::Teleport: {
			if (envelope.Operation != BusOperation::Send) {
				reply.Status = BusStatus::Unsupported;
				break;
			}

			const WorldId destination = directory.Find(envelope.Key);
			if (!destination.IsValid()) {
				reply.Status = BusStatus::NoSuchWorld;
				break;
			}

			// The payload crosses, not an entity. The destination rebuilds
			// the player from its own class definitions, which is what
			// keeps two worlds from having to agree on class versions.
			Delivery arrival;
			arrival.Bus = BusKind::Teleport;
			arrival.Key = sender.Name();
			arrival.From = sender.Name();
			arrival.Payload = envelope.Payload;
			Deliver(destination, std::move(arrival));
			break;
		}
		}

		if (envelope.Reply.Expected()) {
			Deliver(sender.Id(), std::move(reply));
		}
	}

	BarrierCounts BusRouter::Route(const WorldDirectory &directory, const UniverseSettings &settings) {
		// Resized and cleared, never reassigned. `assign(N, {})` destroys every
		// per-world vector and default-constructs a replacement, which throws
		// away the capacity each world built up — once per world per tick,
		// forever. Clearing keeps the buffer, which is this version's standing
		// discipline: preallocate and reuse by default.
		Fanout.resize(directory.Registry.size());
		for (std::vector<Delivery> &pending : Fanout) {
			pending.clear();
		}

		// Every pending envelope from every world, stamped with its sender and
		// sorted by `(From, Sequence)`.
		//
		// The sort is what makes this deterministic: two worlds publishing in
		// the same tick would otherwise be applied in whatever order the world
		// list happened to be walked in, and a replay would diverge. Each
		// world's outbox is already ordered by construction, so this is a merge
		// of sorted runs rather than a general sort — and `std::stable_sort`
		// over a nearly-sorted range is close to linear.
		std::vector<std::pair<World *, Envelope>> traffic;

		BarrierCounts counts;

		if (Replaying) {
			// A replayed world re-derives the same requests it made the first
			// time, so its outbox is discarded rather than merged — applying
			// both copies would double every operation.
			for (const auto &world : directory.Registry) {
				if (world == nullptr) {
					continue;
				}
				world->Storage().BindToCallingThread();
				if (Outbox *outbox = world->Storage().ResourceMutable<Outbox>(); outbox != nullptr) {
					outbox->Pending.clear();
				}
			}

			for (const Envelope &envelope : Injected) {
				World *world = directory.Reach(directory.Find(envelope.From));
				if (world != nullptr) {
					traffic.emplace_back(world, envelope);
				}
			}
			Injected.clear();

			// Already in the order it was applied, so no sort: re-sorting a
			// recording would be trusting this build's comparator over what
			// actually happened.
			Applied.clear();
			for (const auto &[sender, envelope] : traffic) {
				ApplyEnvelope(*sender, envelope, directory);
				Applied.push_back(envelope);
			}
			counts.BusOperations = traffic.size();

			counts.Deliveries = DeliverInboxes(directory, settings);
			return counts;
		}

		for (size_t index = 0; index < directory.Registry.size(); index++) {
			const auto &world = directory.Registry[index];
			if (world == nullptr) {
				continue;
			}
			if (index < directory.Hosts.size() && directory.Hosts[index].IsValid()) {
				// Its outbox is in another process. What it posted arrives
				// through `Ingest` instead.
				continue;
			}

			// Rebound first: the last thread to hold this store was a job
			// worker, and reading a resource from the driver without the
			// handoff is exactly what the affinity check aborts on.
			world->Storage().BindToCallingThread();

			Outbox *outbox = world->Storage().ResourceMutable<Outbox>();
			if (outbox == nullptr || outbox->Pending.empty()) {
				continue;
			}

			for (Envelope &envelope : outbox->Pending) {
				// Stamped by the driver rather than by the sender. A world does
				// not get to say who it is, and every ordering decision here
				// depends on that field.
				envelope.From = world->Name();
				traffic.emplace_back(world.get(), std::move(envelope));
			}
			outbox->Pending.clear();
		}

		if (settings.Federated) {
			// The buses are somebody else's. Collected, stamped, ordered the
			// same way — and then handed up the link instead of applied,
			// because a host that answered its own DataStore read would be a
			// second source of truth for the same key.
			std::stable_sort(traffic.begin(), traffic.end(), [](const auto &left, const auto &right) {
				if (left.second.From.Id() != right.second.From.Id()) {
					return left.second.From.Id() < right.second.From.Id();
				}
				return left.second.Sequence < right.second.Sequence;
			});

			Applied.clear();
			for (auto &[sender, envelope] : traffic) {
				Applied.push_back(std::move(envelope));
			}
			counts.BusOperations = Applied.size();

			counts.Deliveries = DeliverInboxes(directory, settings);
			return counts;
		}

		// What hosts handed over, merged in as though those worlds were local.
		// Their `From` was checked against the host that sent it in `Ingest`, so
		// by here a remote envelope is exactly as trusted as a local one — which
		// is the point: one routing path, not two.
		for (Envelope &envelope : Ingested) {
			World *sender = directory.Reach(directory.Find(envelope.From));
			if (sender != nullptr) {
				traffic.emplace_back(sender, std::move(envelope));
			}
		}
		Ingested.clear();

		std::stable_sort(traffic.begin(), traffic.end(), [](const auto &left, const auto &right) {
			if (left.second.From.Id() != right.second.From.Id()) {
				return left.second.From.Id() < right.second.From.Id();
			}
			return left.second.Sequence < right.second.Sequence;
		});

		Applied.clear();
		for (const auto &[sender, envelope] : traffic) {
			ApplyEnvelope(*sender, envelope, directory);

			// Retained in applied order, which is what a recording records. A
			// replay that re-sorted would be trusting this build's comparator
			// over what actually happened.
			Applied.push_back(envelope);
		}
		counts.BusOperations = traffic.size();

		counts.Deliveries = DeliverInboxes(directory, settings);
		return counts;
	}

	uint64_t BusRouter::DeliverInboxes(const WorldDirectory &directory, const UniverseSettings &settings) {
		// Replacing rather than appending: a system that forgets to drain its
		// inbox misses messages, which is visible, rather than accumulating an
		// unbounded backlog, which is not.
		uint64_t delivered = 0;

		for (size_t index = 0; index < directory.Registry.size(); index++) {
			if (directory.Registry[index] == nullptr) {
				continue;
			}

			if (index < directory.Hosts.size() && directory.Hosts[index].IsValid()) {
				// A world that lives elsewhere has no inbox here. What the
				// barrier decided for it goes out to its host instead, and the
				// count is the same count — a delivery is a delivery whichever
				// process ends up holding it.
				delivered += Fanout[index].size();
				for (Delivery &delivery : Fanout[index]) {
					Outgoing.push_back(
						RemoteDelivery{
							directory.Hosts[index], directory.Registry[index]->Name(), std::move(delivery)
						}
					);
				}
				Fanout[index].clear();
				continue;
			}

			ecs::Store &store = directory.Registry[index]->Storage();
			store.BindToCallingThread();

			delivered += Fanout[index].size();

			// Swapped, not assigned. `SetResource` copies, and a resource
			// column holding a non-trivial type *destroys and re-copies* on
			// assignment — so handing over an inbox this way used to free the
			// world's delivery buffer and allocate a new one every barrier, per
			// world, whether or not anything had arrived.
			//
			// Swapping gives the world its arrivals and gives the fanout the
			// world's old buffer, which is cleared at the next barrier. Neither
			// side allocates once the high-water mark is reached, and the
			// observable behaviour is unchanged: an inbox is still replaced
			// wholesale rather than appended to, so a system that forgets to
			// drain it still misses messages rather than accumulating them.
			if (Inbox *inbox = store.ResourceMutable<Inbox>(); inbox != nullptr) {
				inbox->Arrived.swap(Fanout[index]);
			} else {
				// First barrier for this world. One copy, once.
				Inbox first;
				first.Arrived = std::move(Fanout[index]);
				store.SetResource(first);
			}

			// The budget is per tick, so it resets where every other per-tick
			// thing does. Written in place for the same reason: `SetResource`
			// is a hash lookup and a copy, and this one runs per world per
			// tick to move eight bytes.
			if (BusBudget *budget = store.ResourceMutable<BusBudget>(); budget != nullptr) {
				budget->PerTick = settings.BusBudgetPerTick;
				budget->Spent = 0;
			} else {
				BusBudget first;
				first.PerTick = settings.BusBudgetPerTick;
				store.SetResource(first);
			}
		}

		return delivered;
	}

	void BusRouter::Inject(std::vector<Envelope> traffic) {
		Injected = std::move(traffic);
		Replaying = true;
	}

	size_t
	BusRouter::Ingest(core::Name host, std::span<const Envelope> traffic, const WorldDirectory &directory) {
		size_t accepted = 0;
		for (const Envelope &envelope : traffic) {
			const WorldId sender = directory.Find(envelope.From);

			// `Envelope::From` is stamped rather than trusted, and this is that
			// rule carried across a process boundary. A host that could claim
			// to be a world it does not hold could read that world's replies
			// and publish in its name.
			if (!sender.IsValid() || directory.HostOf(sender) != host) {
				ENGINE_WARN(
					"host '{}' sent traffic claiming to be world '{}', which it does not hold.",
					host.Text(),
					envelope.From.Text()
				);
				continue;
			}

			Ingested.push_back(envelope);
			accepted++;
		}
		return accepted;
	}

	std::vector<RemoteDelivery> BusRouter::TakeOutbound() {
		std::vector<RemoteDelivery> taken;
		taken.swap(Outgoing);
		return taken;
	}

	size_t BusRouter::SubscriberCount(core::Name topic) const {
		const auto found = Backends.Messaging.Subscribers.find(topic.Id());
		return found == Backends.Messaging.Subscribers.end() ? 0 : found->second.size();
	}

	BusStatus BusRouter::Peek(BusKind bus, core::Name key, std::vector<std::byte> *value) const {
		if (bus == BusKind::MemoryStore) {
			const auto found = Backends.Memory.Values.find(key.Id());
			if (found == Backends.Memory.Values.end()) {
				return BusStatus::NotFound;
			}
			if (value != nullptr) {
				*value = found->second;
			}
			return BusStatus::Ok;
		}

		if (bus == BusKind::DataStore) {
			const auto found = Backends.Data.Records.find(key.Id());
			if (found == Backends.Data.Records.end()) {
				return BusStatus::NotFound;
			}
			if (value != nullptr) {
				*value = found->second.Value;
			}
			return BusStatus::Ok;
		}

		return BusStatus::Unsupported;
	}

	void BusRouter::Reset() {
		Backends = Buses{};
		Applied.clear();
		Injected.clear();
		Ingested.clear();
		Outgoing.clear();
	}

	// --- the snapshot codec ------------------------------------------------

	namespace {
		// The keys of a map, ordered by their *text*.
		//
		// Not by name id: an id is assigned in interning order, and a universe
		// restored from a snapshot interns in a different order than the one
		// that wrote it. Sorting by id therefore made a re-save differ from the
		// original byte for byte, which is exactly the property a recording
		// needs to be comparable.
		template <class Map> std::vector<uint32_t> SortedKeys(const Map &map) {
			std::vector<uint32_t> keys;
			keys.reserve(map.size());
			for (const auto &entry : map) {
				keys.push_back(entry.first);
			}
			std::sort(keys.begin(), keys.end(), [](uint32_t left, uint32_t right) {
				return core::Name::FromId(left).Text() < core::Name::FromId(right).Text();
			});
			return keys;
		}

		void WriteBytes(core::ByteWriter &writer, const std::vector<std::byte> &bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}

		std::vector<std::byte> ReadBytes(core::ByteReader &reader) {
			const uint32_t size = reader.ReadUInt32();
			std::vector<std::byte> bytes(reader.Failed() ? 0 : size);
			if (!bytes.empty()) {
				reader.ReadRaw(bytes.data(), bytes.size());
			}
			return bytes;
		}
	}

	void BusRouter::WriteBuses(core::ByteWriter &writer, const WorldDirectory &directory) const {
		// Subscribers by world *name*, never by index: an index means something
		// different in the process that reads this.
		const std::vector<uint32_t> topics = SortedKeys(Backends.Messaging.Subscribers);
		writer.WriteUInt32(static_cast<uint32_t>(topics.size()));
		for (const uint32_t topic : topics) {
			writer.WriteName(core::Name::FromId(topic));

			// Only the ones that still exist, so a restored universe does not
			// carry subscriptions for worlds nobody will recreate. Sorted by
			// name for the same reason the topics are.
			std::vector<std::string_view> names;
			for (const uint32_t index : Backends.Messaging.Subscribers.at(topic)) {
				if (index < directory.Registry.size() && directory.Registry[index] != nullptr) {
					names.push_back(directory.Registry[index]->Name().Text());
				}
			}
			std::sort(names.begin(), names.end());

			writer.WriteUInt32(static_cast<uint32_t>(names.size()));
			for (const std::string_view name : names) {
				writer.WriteString(name);
			}
		}

		const std::vector<uint32_t> values = SortedKeys(Backends.Memory.Values);
		writer.WriteUInt32(static_cast<uint32_t>(values.size()));
		for (const uint32_t key : values) {
			writer.WriteName(core::Name::FromId(key));
			WriteBytes(writer, Backends.Memory.Values.at(key));
		}

		const std::vector<uint32_t> queues = SortedKeys(Backends.Memory.Queues);
		writer.WriteUInt32(static_cast<uint32_t>(queues.size()));
		for (const uint32_t key : queues) {
			writer.WriteName(core::Name::FromId(key));
			const auto &entries = Backends.Memory.Queues.at(key);
			writer.WriteUInt32(static_cast<uint32_t>(entries.size()));
			for (const auto &entry : entries) {
				WriteBytes(writer, entry);
			}
		}

		const std::vector<uint32_t> records = SortedKeys(Backends.Data.Records);
		writer.WriteUInt32(static_cast<uint32_t>(records.size()));
		for (const uint32_t key : records) {
			writer.WriteName(core::Name::FromId(key));
			writer.WriteUInt64(Backends.Data.Records.at(key).Version);
			WriteBytes(writer, Backends.Data.Records.at(key).Value);
		}
	}

	void BusRouter::ReadBuses(core::ByteReader &reader, const WorldDirectory &directory) {
		const uint32_t topics = reader.ReadUInt32();
		for (uint32_t index = 0; index < topics && !reader.Failed(); index++) {
			const core::Name topic = reader.ReadName();
			const uint32_t listeners = reader.ReadUInt32();

			for (uint32_t listener = 0; listener < listeners && !reader.Failed(); listener++) {
				const WorldId id = directory.Find(core::Name(reader.ReadString()));
				if (id.IsValid()) {
					Backends.Messaging.Subscribers[topic.Id()].insert(id.Index);
				}
			}
		}

		const uint32_t values = reader.ReadUInt32();
		for (uint32_t index = 0; index < values && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			Backends.Memory.Values[key.Id()] = ReadBytes(reader);
		}

		const uint32_t queues = reader.ReadUInt32();
		for (uint32_t index = 0; index < queues && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			const uint32_t entries = reader.ReadUInt32();
			for (uint32_t entry = 0; entry < entries && !reader.Failed(); entry++) {
				Backends.Memory.Queues[key.Id()].push_back(ReadBytes(reader));
			}
		}

		const uint32_t records = reader.ReadUInt32();
		for (uint32_t index = 0; index < records && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			DataBus::Record record;
			record.Version = reader.ReadUInt64();
			record.Value = ReadBytes(reader);
			Backends.Data.Records[key.Id()] = std::move(record);
		}
	}
}
