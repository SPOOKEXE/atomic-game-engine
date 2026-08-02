#include "Buses.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace engine::world {

	Universe::Universe(const UniverseSettings &settings)
		: Settings_(settings), BusState(std::make_unique<Buses>()), Driver(std::this_thread::get_id()) {
		// The mailbox resource types need serialisers of their own — an outbox
		// holds a vector and a Name, neither of which survives being written as
		// its object representation.
		RegisterMailboxTypes();
	}

	Universe::~Universe() = default;

	bool Universe::IsOnDriverThread() const {
		return std::this_thread::get_id() == Driver;
	}

	void Universe::RequireDriverThread(const char *what) const {
		if (IsOnDriverThread()) {
			return;
		}

		// Aborts rather than throws, for the same reason the store's affinity
		// check does: by the time this fires the race has happened, and
		// unwinding would hand the corrupted state to whoever caught it.
		ENGINE_ERROR(
			"universe: {} called from a thread that is not the driver. Structural "
			"changes happen at the barrier, on one thread.",
			what
		);
		std::abort();
	}

	World *Universe::Reach(WorldId id) {
		if (!id.IsValid() || id.Index >= Registry.size()) {
			return nullptr;
		}
		return Registry[id.Index].get();
	}

	const World *Universe::Reach(WorldId id) const {
		if (!id.IsValid() || id.Index >= Registry.size()) {
			return nullptr;
		}
		return Registry[id.Index].get();
	}

	WorldId Universe::Adopt(const WorldSettings &settings, core::Name host) {
		// A hole from a destroyed world is reused, so a universe that creates
		// and destroys instanced subareas all day does not grow its registry
		// forever. Handles are not recycled into stale references because a
		// destroyed world's handle stops resolving the moment its slot is null.
		const auto place = [this, &settings, host](size_t index) {
			const WorldId id{static_cast<uint32_t>(index)};
			Registry[index] = std::make_unique<World>(id, settings);
			Hosts[index] = host;

			if (host.IsValid()) {
				// Never ticked here, and its store is never read. The driver
				// holds it so that the buses and the directory still know it
				// exists.
				Registry[index]->SetState(WorldState::Remote);
			}
			return id;
		};

		Hosts.resize(Registry.size());

		for (size_t index = 0; index < Registry.size(); index++) {
			if (Registry[index] == nullptr) {
				return place(index);
			}
		}

		Registry.emplace_back();
		Hosts.emplace_back();
		return place(Registry.size() - 1);
	}

	WorldId Universe::Create(const WorldSettings &settings, WorldStatus *status) {
		RequireDriverThread("Create");

		const auto report = [status](WorldStatus value) {
			if (status != nullptr) {
				*status = value;
			}
		};

		if (!settings.Name.IsValid() || settings.Name.Text().empty()) {
			// Everything that crosses a boundary addresses a world by name, so
			// a world without one cannot be reached by anything outside it.
			report(WorldStatus::NoName);
			return WorldId{};
		}

		if (const WorldId existing = Find(settings.Name); existing.IsValid()) {
			report(WorldStatus::NameTaken);
			return existing;
		}

		if (Ticking) {
			// Queued rather than applied. A registry that grew underneath a
			// running batch would move the very worlds the batch is iterating.
			Control control;
			control.What = Control::Kind::Create;
			control.Settings = settings;
			Pending.push_back(control);

			report(WorldStatus::Ok);
			return WorldId{};
		}

		report(WorldStatus::Ok);
		return Adopt(settings);
	}

	WorldId Universe::CreateRemote(const WorldSettings &settings, core::Name host, WorldStatus *status) {
		RequireDriverThread("CreateRemote");

		const auto report = [status](WorldStatus value) {
			if (status != nullptr) {
				*status = value;
			}
		};

		if (!settings.Name.IsValid() || settings.Name.Text().empty()) {
			report(WorldStatus::NoName);
			return WorldId{};
		}
		if (!host.IsValid()) {
			// A remote world with no host is a name the driver can address and
			// can never reach, which is worse than not having it.
			report(WorldStatus::NoName);
			return WorldId{};
		}

		if (const WorldId existing = Find(settings.Name); existing.IsValid()) {
			report(WorldStatus::NameTaken);
			return existing;
		}

		if (Ticking) {
			Control control;
			control.What = Control::Kind::Create;
			control.Settings = settings;
			control.Host = host;
			Pending.push_back(control);

			report(WorldStatus::Ok);
			return WorldId{};
		}

		report(WorldStatus::Ok);
		return Adopt(settings, host);
	}

	core::Name Universe::HostOf(WorldId id) const {
		if (Reach(id) == nullptr || id.Index >= Hosts.size()) {
			return {};
		}
		return Hosts[id.Index];
	}

	bool Universe::IsRemote(WorldId id) const {
		return HostOf(id).IsValid();
	}

	WorldStatus Universe::Destroy(WorldId id) {
		RequireDriverThread("Destroy");

		if (Reach(id) == nullptr) {
			return WorldStatus::NoSuchWorld;
		}

		if (Ticking) {
			Control control;
			control.What = Control::Kind::Destroy;
			control.Target = id;
			Pending.push_back(control);
			return WorldStatus::Ok;
		}

		Registry[id.Index].reset();
		if (id.Index < Hosts.size()) {
			Hosts[id.Index] = core::Name{};
		}
		return WorldStatus::Ok;
	}

	WorldStatus Universe::SetState(WorldId id, WorldState state) {
		RequireDriverThread("SetState");

		World *world = Reach(id);
		if (world == nullptr) {
			return WorldStatus::NoSuchWorld;
		}

		if (Ticking) {
			Control control;
			control.What = Control::Kind::SetState;
			control.Target = id;
			control.State = state;
			Pending.push_back(control);
			return WorldStatus::Ok;
		}

		world->SetState(state);
		return WorldStatus::Ok;
	}

	WorldStatus Universe::Recover(WorldId id) {
		RequireDriverThread("Recover");

		World *world = Reach(id);
		if (world == nullptr) {
			return WorldStatus::NoSuchWorld;
		}

		if (Ticking) {
			Control control;
			control.What = Control::Kind::Recover;
			control.Target = id;
			Pending.push_back(control);
			return WorldStatus::Ok;
		}

		world->Recover();
		return WorldStatus::Ok;
	}

	void Universe::Apply(const Control &control) {
		switch (control.What) {
		case Control::Kind::Create:
			if (!Find(control.Settings.Name).IsValid()) {
				Adopt(control.Settings, control.Host);
			}
			break;

		case Control::Kind::Destroy:
			if (control.Target.IsValid() && control.Target.Index < Registry.size()) {
				Registry[control.Target.Index].reset();
				if (control.Target.Index < Hosts.size()) {
					Hosts[control.Target.Index] = core::Name{};
				}
			}
			break;

		case Control::Kind::SetState:
			if (World *world = Reach(control.Target); world != nullptr) {
				world->SetState(control.State);
			}
			break;

		case Control::Kind::Recover:
			if (World *world = Reach(control.Target); world != nullptr) {
				world->Recover();
			}
			break;
		}
	}

	void Universe::DrainControls() {
		if (Pending.empty()) {
			return;
		}

		// Taken by move before replaying, because applying one may queue
		// another — a create whose settings name a world that is also being
		// destroyed this barrier.
		std::vector<Control> controls;
		controls.swap(Pending);

		for (const Control &control : controls) {
			Apply(control);
		}
	}

	WorldId Universe::Find(core::Name name) const {
		for (size_t index = 0; index < Registry.size(); index++) {
			if (Registry[index] != nullptr && Registry[index]->Name() == name) {
				return WorldId{static_cast<uint32_t>(index)};
			}
		}
		return WorldId{};
	}

	size_t Universe::Count() const {
		size_t count = 0;
		for (const auto &world : Registry) {
			if (world != nullptr) {
				count++;
			}
		}
		return count;
	}

	std::vector<WorldId> Universe::Worlds() const {
		std::vector<WorldId> handles;
		for (size_t index = 0; index < Registry.size(); index++) {
			if (Registry[index] != nullptr) {
				handles.push_back(WorldId{static_cast<uint32_t>(index)});
			}
		}
		return handles;
	}

	core::Name Universe::NameOf(WorldId id) const {
		const World *world = Reach(id);
		return world == nullptr ? core::Name{} : world->Name();
	}

	WorldState Universe::StateOf(WorldId id) const {
		const World *world = Reach(id);
		return world == nullptr ? WorldState::Suspended : world->State();
	}

	float Universe::AlphaOf(WorldId id) const {
		const World *world = Reach(id);
		return world == nullptr ? 0.0f : world->Alpha();
	}

	WorldStatistics Universe::StatisticsOf(WorldId id) const {
		const World *world = Reach(id);
		return world == nullptr ? WorldStatistics{} : world->Statistics();
	}

	void Universe::SetMode(ExecutionMode mode) {
		RequireDriverThread("SetMode");
		Settings_.Mode = mode;
	}

	void Universe::Deliver(WorldId id, Delivery delivery) {
		if (!id.IsValid() || id.Index >= Fanout.size()) {
			return;
		}
		Fanout[id.Index].push_back(std::move(delivery));
	}

	void Universe::ApplyEnvelope(World &sender, const Envelope &envelope, std::vector<Delivery> *) {
		const uint32_t key = envelope.Key.Id();
		const uint32_t from = sender.Name().Id();

		// The reply this operation produces, if it asked for one. Built here
		// and posted at the end, so every path fills the same record.
		Delivery reply;
		reply.Bus = envelope.Bus;
		reply.Key = envelope.Key;
		reply.Reply = envelope.Reply;
		reply.Status = BusStatus::Ok;

		switch (envelope.Bus) {
		case BusKind::Messaging: {
			auto &topics = BusState->Messaging.Subscribers;

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
			auto &values = BusState->Memory.Values;
			auto &queues = BusState->Memory.Queues;

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
			auto &records = BusState->Data.Records;

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

			const WorldId destination = Find(envelope.Key);
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

		(void)from;
	}

	void Universe::RouteBuses() {
		// Resized and cleared, never reassigned. `assign(N, {})` destroys every
		// per-world vector and default-constructs a replacement, which throws
		// away the capacity each world built up — once per world per tick,
		// forever. Clearing keeps the buffer, which is this version's standing
		// discipline: preallocate and reuse by default.
		Fanout.resize(Registry.size());
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

		if (Replaying) {
			// A replayed world re-derives the same requests it made the first
			// time, so its outbox is discarded rather than merged — applying
			// both copies would double every operation.
			for (auto &world : Registry) {
				if (world == nullptr) {
					continue;
				}
				world->Storage().BindToCallingThread();
				if (Outbox *outbox = world->Storage().ResourceMutable<Outbox>(); outbox != nullptr) {
					outbox->Pending.clear();
				}
			}

			for (const Envelope &envelope : Injected) {
				const WorldId sender = Find(envelope.From);
				World *world = Reach(sender);
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
				ApplyEnvelope(*sender, envelope, nullptr);
				Applied.push_back(envelope);
			}
			Stats.BusOperations = traffic.size();

			DeliverInboxes();
			return;
		}

		for (size_t index = 0; index < Registry.size(); index++) {
			auto &world = Registry[index];
			if (world == nullptr) {
				continue;
			}
			if (index < Hosts.size() && Hosts[index].IsValid()) {
				// Its outbox is in another process. What it posted arrives
				// through `IngestTraffic` instead.
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

		if (Settings_.Federated) {
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
			Stats.BusOperations = Applied.size();

			DeliverInboxes();
			return;
		}

		// What hosts handed over, merged in as though those worlds were local.
		// Their `From` was checked against the host that sent it in
		// `IngestTraffic`, so by here a remote envelope is exactly as trusted
		// as a local one — which is the point: one routing path, not two.
		for (Envelope &envelope : Ingested) {
			World *sender = Reach(Find(envelope.From));
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
			ApplyEnvelope(*sender, envelope, nullptr);

			// Retained in applied order, which is what a recording records. A
			// replay that re-sorted would be trusting this build's comparator
			// over what actually happened.
			Applied.push_back(envelope);
		}
		Stats.BusOperations = traffic.size();

		DeliverInboxes();
	}

	void Universe::DeliverInboxes() {
		// Replacing rather than appending: a system that forgets to drain its
		// inbox misses messages, which is visible, rather than accumulating an
		// unbounded backlog, which is not.
		Stats.Deliveries = 0;

		for (size_t index = 0; index < Registry.size(); index++) {
			if (Registry[index] == nullptr) {
				continue;
			}

			if (index < Hosts.size() && Hosts[index].IsValid()) {
				// A world that lives elsewhere has no inbox here. What the
				// barrier decided for it goes out to its host instead, and the
				// count is the same count — a delivery is a delivery whichever
				// process ends up holding it.
				Stats.Deliveries += Fanout[index].size();
				for (Delivery &delivery : Fanout[index]) {
					Pending_.push_back(
						RemoteDelivery{Hosts[index], Registry[index]->Name(), std::move(delivery)}
					);
				}
				Fanout[index].clear();
				continue;
			}

			ecs::Store &store = Registry[index]->Storage();
			store.BindToCallingThread();

			Stats.Deliveries += Fanout[index].size();

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
				budget->PerTick = Settings_.BusBudgetPerTick;
				budget->Spent = 0;
			} else {
				BusBudget first;
				first.PerTick = Settings_.BusBudgetPerTick;
				store.SetResource(first);
			}
		}
	}

	std::span<const Envelope> Universe::LastTraffic() const {
		return {Applied.data(), Applied.size()};
	}

	size_t Universe::IngestTraffic(core::Name host, std::span<const Envelope> traffic) {
		RequireDriverThread("IngestTraffic");

		size_t accepted = 0;
		for (const Envelope &envelope : traffic) {
			const WorldId sender = Find(envelope.From);

			// `Envelope::From` is stamped rather than trusted, and this is that
			// rule carried across a process boundary. A host that could claim
			// to be a world it does not hold could read that world's replies
			// and publish in its name.
			if (!sender.IsValid() || HostOf(sender) != host) {
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

	std::vector<RemoteDelivery> Universe::TakeOutbound() {
		std::vector<RemoteDelivery> taken;
		taken.swap(Pending_);
		return taken;
	}

	bool Universe::Deliver(core::Name world, const Delivery &delivery) {
		RequireDriverThread("Deliver");

		const WorldId id = Find(world);
		if (!id.IsValid() || id.Index >= Fanout.size()) {
			return false;
		}

		Fanout[id.Index].push_back(delivery);
		return true;
	}

	void Universe::InjectTraffic(std::vector<Envelope> traffic) {
		RequireDriverThread("InjectTraffic");
		Injected = std::move(traffic);
		Replaying = true;
	}

	size_t Universe::SubscriberCount(core::Name topic) const {
		const auto found = BusState->Messaging.Subscribers.find(topic.Id());
		return found == BusState->Messaging.Subscribers.end() ? 0 : found->second.size();
	}

	BusStatus Universe::Peek(BusKind bus, core::Name key, std::vector<std::byte> *value) const {
		if (bus == BusKind::MemoryStore) {
			const auto found = BusState->Memory.Values.find(key.Id());
			if (found == BusState->Memory.Values.end()) {
				return BusStatus::NotFound;
			}
			if (value != nullptr) {
				*value = found->second;
			}
			return BusStatus::Ok;
		}

		if (bus == BusKind::DataStore) {
			const auto found = BusState->Data.Records.find(key.Id());
			if (found == BusState->Data.Records.end()) {
				return BusStatus::NotFound;
			}
			if (value != nullptr) {
				*value = found->second.Value;
			}
			return BusStatus::Ok;
		}

		return BusStatus::Unsupported;
	}

	// --- snapshots ---------------------------------------------------------

	namespace {
		// Recognises a universe snapshot before anything else is read.
		constexpr uint64_t UNIVERSE_MAGIC = 0x5649'4E55'4F4E'4F4Dull;

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

	bool Universe::Save(core::ByteWriter &writer) const {
		RequireDriverThread("Save");

		writer.WriteUInt64(UNIVERSE_MAGIC);
		writer.WriteUInt32(SNAPSHOT_VERSION);

		writer.WriteUInt8(static_cast<uint8_t>(Settings_.Mode));
		writer.WriteInt32(Settings_.MaximumCatchUpTicks);
		writer.WriteUInt32(Settings_.BusBudgetPerTick);

		// --- worlds ---
		writer.WriteUInt32(static_cast<uint32_t>(Count()));
		for (size_t index = 0; index < Registry.size(); index++) {
			const auto &world = Registry[index];
			if (world == nullptr) {
				continue;
			}

			writer.WriteName(world->Name());
			writer.WriteDouble(world->Settings().TickRate);
			writer.WriteDouble(world->Settings().IdleTickRate);
			writer.WriteUInt8(static_cast<uint8_t>(world->Settings().IsolationLevel));
			writer.WriteUInt32(world->Settings().FaultLimit);
			writer.WriteUInt8(static_cast<uint8_t>(world->State()));

			// The host holding it, or an invalid Name for a world this process
			// holds. Without it a restored driver would bring every remote
			// world back as a local one — with an empty store, ticking, and
			// answering for a world that is still running somewhere else.
			writer.WriteName(index < Hosts.size() ? Hosts[index] : core::Name{});

			// Length-prefixed, so a reader that does not understand a world can
			// skip it rather than losing its place in the stream.
			core::ByteWriter storage;
			if (!const_cast<World *>(world.get())->Storage().Save(storage)) {
				return false;
			}
			writer.WriteUInt32(static_cast<uint32_t>(storage.Size()));
			writer.WriteRaw(storage.Bytes().data(), storage.Size());
		}

		// --- buses ---
		//
		// Subscribers by world *name*, never by index: an index means something
		// different in the process that reads this.
		const std::vector<uint32_t> topics = SortedKeys(BusState->Messaging.Subscribers);
		writer.WriteUInt32(static_cast<uint32_t>(topics.size()));
		for (const uint32_t topic : topics) {
			writer.WriteName(core::Name::FromId(topic));

			// Only the ones that still exist, so a restored universe does not
			// carry subscriptions for worlds nobody will recreate. Sorted by
			// name for the same reason the topics are.
			std::vector<std::string_view> names;
			for (const uint32_t index : BusState->Messaging.Subscribers.at(topic)) {
				if (index < Registry.size() && Registry[index] != nullptr) {
					names.push_back(Registry[index]->Name().Text());
				}
			}
			std::sort(names.begin(), names.end());

			writer.WriteUInt32(static_cast<uint32_t>(names.size()));
			for (const std::string_view name : names) {
				writer.WriteString(name);
			}
		}

		const std::vector<uint32_t> values = SortedKeys(BusState->Memory.Values);
		writer.WriteUInt32(static_cast<uint32_t>(values.size()));
		for (const uint32_t key : values) {
			writer.WriteName(core::Name::FromId(key));
			WriteBytes(writer, BusState->Memory.Values.at(key));
		}

		const std::vector<uint32_t> queues = SortedKeys(BusState->Memory.Queues);
		writer.WriteUInt32(static_cast<uint32_t>(queues.size()));
		for (const uint32_t key : queues) {
			writer.WriteName(core::Name::FromId(key));
			const auto &entries = BusState->Memory.Queues.at(key);
			writer.WriteUInt32(static_cast<uint32_t>(entries.size()));
			for (const auto &entry : entries) {
				WriteBytes(writer, entry);
			}
		}

		const std::vector<uint32_t> records = SortedKeys(BusState->Data.Records);
		writer.WriteUInt32(static_cast<uint32_t>(records.size()));
		for (const uint32_t key : records) {
			writer.WriteName(core::Name::FromId(key));
			writer.WriteUInt64(BusState->Data.Records.at(key).Version);
			WriteBytes(writer, BusState->Data.Records.at(key).Value);
		}

		return true;
	}

	bool Universe::Load(core::ByteReader &reader) {
		RequireDriverThread("Load");

		const auto abandon = [this] {
			Registry.clear();
			Hosts.clear();
			Pending.clear();
			Applied.clear();
			Injected.clear();
			Ingested.clear();
			Pending_.clear();
			*BusState = Buses{};
			return false;
		};

		abandon();

		if (reader.ReadUInt64() != UNIVERSE_MAGIC) {
			ENGINE_ERROR("universe: not a snapshot.");
			return abandon();
		}

		const uint32_t version = reader.ReadUInt32();
		if (version != SNAPSHOT_VERSION) {
			ENGINE_ERROR("universe: snapshot version {}, this build reads {}.", version, SNAPSHOT_VERSION);
			return abandon();
		}

		Settings_.Mode = static_cast<ExecutionMode>(reader.ReadUInt8());
		Settings_.MaximumCatchUpTicks = reader.ReadInt32();
		Settings_.BusBudgetPerTick = reader.ReadUInt32();

		const uint32_t worlds = reader.ReadUInt32();
		for (uint32_t index = 0; index < worlds && !reader.Failed(); index++) {
			WorldSettings settings;
			settings.Name = reader.ReadName();
			settings.TickRate = reader.ReadDouble();
			settings.IdleTickRate = reader.ReadDouble();
			settings.IsolationLevel = static_cast<Isolation>(reader.ReadUInt8());
			settings.FaultLimit = reader.ReadUInt32();

			const auto state = static_cast<WorldState>(reader.ReadUInt8());
			const core::Name host = reader.ReadName();
			const std::vector<std::byte> storage = ReadBytes(reader);
			if (reader.Failed() || !settings.Name.IsValid()) {
				return abandon();
			}

			const WorldId id = Adopt(settings, host);
			World *world = Reach(id);

			core::ByteReader worldReader(storage);
			if (!world->Storage().Load(worldReader)) {
				return abandon();
			}
			world->SetState(state);
		}

		const uint32_t topics = reader.ReadUInt32();
		for (uint32_t index = 0; index < topics && !reader.Failed(); index++) {
			const core::Name topic = reader.ReadName();
			const uint32_t listeners = reader.ReadUInt32();

			for (uint32_t listener = 0; listener < listeners && !reader.Failed(); listener++) {
				const WorldId id = Find(core::Name(reader.ReadString()));
				if (id.IsValid()) {
					BusState->Messaging.Subscribers[topic.Id()].insert(id.Index);
				}
			}
		}

		const uint32_t values = reader.ReadUInt32();
		for (uint32_t index = 0; index < values && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			BusState->Memory.Values[key.Id()] = ReadBytes(reader);
		}

		const uint32_t queues = reader.ReadUInt32();
		for (uint32_t index = 0; index < queues && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			const uint32_t entries = reader.ReadUInt32();
			for (uint32_t entry = 0; entry < entries && !reader.Failed(); entry++) {
				BusState->Memory.Queues[key.Id()].push_back(ReadBytes(reader));
			}
		}

		const uint32_t records = reader.ReadUInt32();
		for (uint32_t index = 0; index < records && !reader.Failed(); index++) {
			const core::Name key = reader.ReadName();
			DataBus::Record record;
			record.Version = reader.ReadUInt64();
			record.Value = ReadBytes(reader);
			BusState->Data.Records[key.Id()] = std::move(record);
		}

		if (reader.Failed()) {
			return abandon();
		}

		return true;
	}

	void Universe::Tick(float frameSeconds) {
		RequireDriverThread("Tick");
		ENGINE_PROFILE_CAT("Universe::Tick", engine::core::ProfileCategory::Simulation);

		const uint64_t started = core::Clock::Nanoseconds();

		// --- 1. the barrier ---
		DrainControls();
		RouteBuses();

		// --- 2. who owes a tick ---
		ActiveList.clear();
		OwedList.clear();

		for (const auto &world : Registry) {
			if (world == nullptr) {
				continue;
			}

			int owed = world->Owed(frameSeconds);
			if (owed <= 0) {
				continue;
			}

			if (owed > Settings_.MaximumCatchUpTicks) {
				// A world far enough behind will not recover by running a
				// hundred ticks in one frame; it will only fall further behind
				// while holding a worker. Dropping the excess and counting it
				// makes that visible instead of terminal.
				owed = Settings_.MaximumCatchUpTicks;
			}

			ActiveList.push_back(world.get());
			OwedList.push_back(owed);
		}

		// Longest first, by what the world's last tick cost. `Jobs::For` claims
		// ranges in order, so starting with the expensive ones keeps the tail
		// of the batch short — the cheap worlds fill in behind them rather than
		// one long world finishing alone.
		std::vector<size_t> order(ActiveList.size());
		for (size_t index = 0; index < order.size(); index++) {
			order[index] = index;
		}
		std::sort(order.begin(), order.end(), [this](size_t left, size_t right) {
			const float leftCost = ActiveList[left]->Statistics().LastTickMilliseconds;
			const float rightCost = ActiveList[right]->Statistics().LastTickMilliseconds;
			if (leftCost != rightCost) {
				return leftCost > rightCost;
			}
			// Ties broken by index rather than left unspecified, so two runs
			// dispatch in the same order and a recorded run replays.
			return left < right;
		});

		// --- 3. the only parallel step ---
		Ticking = true;

		if (Settings_.Mode == ExecutionMode::WorldParallel && ActiveList.size() > 1) {
			// Grain of one: a world is the unit of work, and there is no
			// smaller division of it. Whichever worker claims a world runs it,
			// which is why the store rebinds every tick.
			parallel::Jobs::For(order.size(), 1, [this, &order](size_t begin, size_t end) {
				for (size_t at = begin; at < end; at++) {
					const size_t index = order[at];
					ActiveList[index]->Tick(OwedList[index]);
				}
			});
		} else {
			for (const size_t index : order) {
				ActiveList[index]->Tick(OwedList[index]);
			}
		}

		Ticking = false;

		// --- 4. diagnostics, and anything the tick queued ---
		DrainControls();

		Stats.ActiveWorlds = ActiveList.size();
		Stats.Suspended = 0;
		Stats.Faulted = 0;
		Stats.Remote = 0;
		Stats.SimulationTicks = 0;

		for (const auto &world : Registry) {
			if (world == nullptr) {
				continue;
			}
			if (world->State() == WorldState::Suspended) {
				Stats.Suspended++;
			}
			if (world->State() == WorldState::Faulted) {
				Stats.Faulted++;
			}
			if (world->State() == WorldState::Remote) {
				Stats.Remote++;
			}
			Stats.SimulationTicks += world->Statistics().Ticks;
		}

		Stats.LastTickMilliseconds =
			static_cast<float>(static_cast<double>(core::Clock::Nanoseconds() - started) / 1'000'000.0);
	}

	WorldStatus Universe::Present(WorldId id, float frameSeconds, float alpha) {
		RequireDriverThread("Present");

		World *world = Reach(id);
		if (world == nullptr) {
			return WorldStatus::NoSuchWorld;
		}

		world->Present(frameSeconds, alpha);
		return WorldStatus::Ok;
	}

	WorldStatus Universe::Enter(WorldId id, const std::function<void(ecs::Store &, ecs::Scheduler &)> &body) {
		RequireDriverThread("Enter");

		if (Ticking) {
			// The world may be mid-tick on a worker, and the store's affinity
			// check would be the only thing between that and a data race.
			ENGINE_ERROR("universe: Enter called while a tick batch is in flight.");
			std::abort();
		}

		World *world = Reach(id);
		if (world == nullptr) {
			return WorldStatus::NoSuchWorld;
		}

		// Rebound because the last thread to hold it was a job worker.
		world->Storage().BindToCallingThread();
		body(world->Storage(), world->Systems());
		return WorldStatus::Ok;
	}

	WorldStatus Universe::Enter(WorldId id, const std::function<void(ecs::Store &)> &body) {
		return Enter(id, [&body](ecs::Store &store, ecs::Scheduler &) { body(store); });
	}
}
