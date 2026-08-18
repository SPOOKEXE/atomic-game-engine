#include "BusRouter.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cstdlib>

namespace engine::world {

	Universe::Universe(const UniverseSettings &settings)
		: Settings_(settings), Router(std::make_unique<BusRouter>()), Driver(std::this_thread::get_id()) {
		// The mailbox resource types need serialisers of their own - an outbox
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

	// The three lookups below live on WorldDirectory rather than here, because
	// the router does them too and two implementations of "which world is this"
	// would agree until the day one of them did not. A directory is a view over
	// `Registry` and `Hosts`, so building one costs two spans.

	World *Universe::Reach(WorldId id) {
		return WorldDirectory{Registry, Hosts}.Reach(id);
	}

	const World *Universe::Reach(WorldId id) const {
		return WorldDirectory{Registry, Hosts}.Reach(id);
	}

	void Universe::RefreshLanes(unsigned laneCount) {
		if (laneCount != LaneCount) {
			LaneCount = laneCount;
			LaneByWorld.assign(Registry.size(), INVALID_LANE);
		} else {
			LaneByWorld.resize(Registry.size(), INVALID_LANE);
		}

		if (laneCount == 0) {
			std::fill(LaneByWorld.begin(), LaneByWorld.end(), INVALID_LANE);
			return;
		}

		std::vector<size_t> laneLoads(laneCount, 0);
		for (size_t index = 0; index < Registry.size(); index++) {
			const bool local =
				Registry[index] != nullptr && (index >= Hosts.size() || !Hosts[index].IsValid());
			if (!local) {
				LaneByWorld[index] = INVALID_LANE;
				continue;
			}
			if (LaneByWorld[index] < laneCount) {
				laneLoads[LaneByWorld[index]]++;
			}
		}

		for (size_t index = 0; index < Registry.size(); index++) {
			const bool local =
				Registry[index] != nullptr && (index >= Hosts.size() || !Hosts[index].IsValid());
			if (!local || LaneByWorld[index] < laneCount) {
				continue;
			}

			const auto lightest = std::min_element(laneLoads.begin(), laneLoads.end());
			const unsigned lane = static_cast<unsigned>(std::distance(laneLoads.begin(), lightest));
			LaneByWorld[index] = lane;
			(*lightest)++;
		}
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
			RefreshLanes(parallel::Jobs::PinnedWorkerCount());
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
		return WorldDirectory{Registry, Hosts}.HostOf(id);
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
		if (id.Index < LaneByWorld.size()) {
			LaneByWorld[id.Index] = INVALID_LANE;
		}
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
				if (control.Target.Index < LaneByWorld.size()) {
					LaneByWorld[control.Target.Index] = INVALID_LANE;
				}
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
		// another - a create whose settings name a world that is also being
		// destroyed this barrier.
		std::vector<Control> controls;
		controls.swap(Pending);

		for (const Control &control : controls) {
			Apply(control);
		}
	}

	WorldId Universe::Find(core::Name name) const {
		return WorldDirectory{Registry, Hosts}.Find(name);
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

	size_t Universe::CountInState(WorldState state) const {
		size_t count = 0;
		for (const auto &world : Registry) {
			if (world != nullptr && world->State() == state) {
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

	WorldSettings Universe::SettingsOf(WorldId id) const {
		const World *world = Reach(id);
		return world == nullptr ? WorldSettings{} : world->Settings();
	}

	WorldStatus Universe::SetRenderingProfile(WorldId id, core::Name profile) {
		RequireDriverThread("SetRenderingProfile");
		World *world = Reach(id);
		if (world == nullptr) {
			return WorldStatus::NoSuchWorld;
		}
		world->SetRenderingProfile(profile);
		return WorldStatus::Ok;
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

	void Universe::SetMaximumCatchUpTicks(int ticks) {
		RequireDriverThread("SetMaximumCatchUpTicks");
		Settings_.MaximumCatchUpTicks = std::max(ticks, 1);
	}

	void Universe::SetBusBudgetPerTick(uint32_t budget) {
		RequireDriverThread("SetBusBudgetPerTick");
		Settings_.BusBudgetPerTick = budget;
	}

	// --- the buses, which BusRouter owns -----------------------------------
	//
	// The thread check stays on this side. It is a rule about who may call a
	// universe, and a router repeating it would be enforcing a rule it cannot
	// see the reason for.

	std::span<const Envelope> Universe::LastTraffic() const {
		return Router->LastTraffic();
	}

	size_t Universe::IngestTraffic(core::Name host, std::span<const Envelope> traffic) {
		RequireDriverThread("IngestTraffic");
		return Router->Ingest(host, traffic, WorldDirectory{Registry, Hosts});
	}

	std::span<const RemoteDelivery> Universe::Outbound() const {
		return Router->Outbound();
	}

	std::vector<RemoteDelivery> Universe::TakeOutbound() {
		return Router->TakeOutbound();
	}

	bool Universe::Deliver(core::Name world, const Delivery &delivery) {
		RequireDriverThread("Deliver");
		return Router->Deliver(Find(world), delivery);
	}

	void Universe::InjectTraffic(std::vector<Envelope> traffic) {
		RequireDriverThread("InjectTraffic");
		Router->Inject(std::move(traffic));
	}

	size_t Universe::SubscriberCount(core::Name topic) const {
		return Router->SubscriberCount(topic);
	}

	BusStatus Universe::Peek(BusKind bus, core::Name key, std::vector<std::byte> *value) const {
		return Router->Peek(bus, key, value);
	}

	// --- snapshots ---------------------------------------------------------

	namespace {
		// Recognises a universe snapshot before anything else is read.
		constexpr uint64_t UNIVERSE_MAGIC = 0x5649'4E55'4F4E'4F4Dull;
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
			writer.WriteDouble(world->Settings().GlobalSimulatedNetworkLatency);
			writer.WriteUInt8(static_cast<uint8_t>(world->Settings().IsolationLevel));
			writer.WriteUInt32(world->Settings().FaultLimit);
			writer.WriteName(world->Settings().RenderingProfile);
			writer.WriteUInt8(static_cast<uint8_t>(world->State()));

			// The host holding it, or an invalid Name for a world this process
			// holds. Without it a restored driver would bring every remote
			// world back as a local one - with an empty store, ticking, and
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
		Router->WriteBuses(writer, WorldDirectory{Registry, Hosts});

		return true;
	}

	bool Universe::Load(core::ByteReader &reader) {
		RequireDriverThread("Load");

		const auto abandon = [this] {
			Registry.clear();
			Hosts.clear();
			LaneByWorld.clear();
			Pending.clear();
			Router->Reset();
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
			settings.GlobalSimulatedNetworkLatency = reader.ReadDouble();
			settings.IsolationLevel = static_cast<Isolation>(reader.ReadUInt8());
			settings.FaultLimit = reader.ReadUInt32();
			settings.RenderingProfile = reader.ReadName();

			const auto state = static_cast<WorldState>(reader.ReadUInt8());
			const core::Name host = reader.ReadName();

			// The other side of the length prefix Save wrote. Sized to nothing
			// when the reader has already failed, because a length read out of
			// a spent buffer is not a length.
			const uint32_t length = reader.ReadUInt32();
			std::vector<std::byte> storage(reader.Failed() ? 0 : length);
			if (!storage.empty()) {
				reader.ReadRaw(storage.data(), storage.size());
			}

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

		// After the worlds, so a subscriber naming one resolves against the
		// registry this snapshot restored rather than against whatever was here
		// before.
		Router->ReadBuses(reader, WorldDirectory{Registry, Hosts});

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
		//
		// `Simulation`, not `ECS`: this is the machinery around the systems
		// rather than the systems themselves, and a driver spending more on its
		// barrier than on its worlds is a real problem that one category could
		// not show.
		{
			ENGINE_PROFILE_CAT("barrier", engine::core::ProfileCategory::Simulation);
			DrainControls();

			const BarrierCounts barrier = Router->Route(WorldDirectory{Registry, Hosts}, Settings_);
			Stats.BusOperations = barrier.BusOperations;
			Stats.Deliveries = barrier.Deliveries;
		}

		// --- 2. who owes a tick ---
		RefreshLanes(parallel::Jobs::PinnedWorkerCount());
		ActiveList.clear();
		OwedList.clear();
		ActiveLanes.clear();

		for (size_t index = 0; index < Registry.size(); index++) {
			const auto &world = Registry[index];
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
			ActiveLanes.push_back(LaneByWorld[index]);
		}

		// Longest first, by what the world's last tick cost. Each lane handles its
		// assigned worlds in this order, so the expensive work begins before the
		// cheap worlds waiting behind it.
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

		DispatchLanes.resize(order.size());
		for (size_t at = 0; at < order.size(); at++) {
			DispatchLanes[at] = ActiveLanes[order[at]];
		}

		// --- 3. the only parallel step ---
		Ticking = true;

		// **The flag is read here as well as inside `Jobs::ForWorkers`, and one is not
		// enough.** Forcing the dispatch inline would already put every world on
		// this thread - but this branch would still report an aggregate
		// `worlds (pinned workers)` bar *beside* the real spans that are now being
		// kept, and the flame graph would count the tick twice. A measurement
		// instrument that makes the picture wrong in a new way is worse than no
		// instrument, so the shape goes serial with the execution.
		const bool parallel =
			Settings_.Mode == ExecutionMode::WorldParallel && !parallel::ForceSerialCompute();

		if (parallel && !ActiveList.empty() && LaneCount > 0) {
			// A world keeps its lane while the pinned worker prefix is unchanged.
			// Excess worlds share a lane and run there in deterministic index
			// order, which preserves the distinct-core guarantee for work
			// that is actually simultaneous without inventing processors.
			parallel::Jobs::ForWorkers(DispatchLanes, [this, &order](size_t begin, size_t end) {
				for (size_t at = begin; at < end; at++) {
					const size_t index = order[at];
					ActiveList[index]->Tick(OwedList[index]);
				}
			});

			// **The workers' time, reported rather than timed from here.**
			// Every span a world opened ran on a worker thread, and the frame
			// graph refuses those - so without this the most expensive thing a
			// driver does shows up only in the drop counter. The workers
			// measured themselves; this is the number they handed back.
			//
			// It is the *sum* across workers, so it exceeds the wall time this
			// scope took whenever more than one of them ran. That is the point:
			// the bar says how much work the tick contained, and the scope
			// around it says how long it took to do. `FrameGraph::Report`
			// keeps the two from being subtracted from each other.
			const parallel::BatchTiming batch = parallel::Jobs::LastBatch();
			core::FrameGraph::Report(
				"worlds (pinned workers)", core::ProfileCategory::ECS, batch.BusyMilliseconds
			);
		} else {
			ENGINE_PROFILE_CAT("worlds (serial)", engine::core::ProfileCategory::ECS);
			for (const size_t index : order) {
				ActiveList[index]->Tick(OwedList[index]);
			}
		}

		Ticking = false;

		// --- 4. diagnostics, and anything the tick queued ---
		// Every worker has joined before the router can touch an outbox. Bus and
		// service traffic therefore crosses cores as copied envelopes at the next
		// driver barrier, never as shared world storage.
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

		if (Reach(id) == nullptr) {
			return WorldStatus::NoSuchWorld;
		}
		if (IsRemote(id)) {
			return WorldStatus::Ok;
		}

		const Presentation request{id, frameSeconds, alpha};
		(void)PresentMany(std::span<const Presentation>(&request, 1));
		return WorldStatus::Ok;
	}

	size_t Universe::PresentMany(std::span<const Presentation> requests) {
		RequireDriverThread("PresentMany");

		if (Ticking) {
			ENGINE_ERROR("universe: PresentMany called while a tick batch is in flight.");
			std::abort();
		}

		RefreshLanes(parallel::Jobs::PinnedWorkerCount());
		PresentationList.clear();
		PresentationRequests.clear();
		PresentationLanes.clear();
		PresentationList.reserve(requests.size());
		PresentationRequests.reserve(requests.size());
		PresentationLanes.reserve(requests.size());
		PresentationQueued.resize(Registry.size());
		std::fill(PresentationQueued.begin(), PresentationQueued.end(), uint8_t{0});

		for (const Presentation &request : requests) {
			World *world = Reach(request.World);
			if (world == nullptr || IsRemote(request.World)) {
				continue;
			}

			if (PresentationQueued[request.World.Index] != 0) {
				continue;
			}
			PresentationQueued[request.World.Index] = 1;

			PresentationList.push_back(world);
			PresentationRequests.push_back(request);
			PresentationLanes.push_back(
				request.World.Index < LaneByWorld.size() ? LaneByWorld[request.World.Index] : INVALID_LANE
			);
		}

		const bool parallel =
			Settings_.Mode == ExecutionMode::WorldParallel && !parallel::ForceSerialCompute() &&
			LaneCount > 0 &&
			std::all_of(PresentationLanes.begin(), PresentationLanes.end(), [this](unsigned lane) {
				return lane < LaneCount;
			});

		if (parallel && !PresentationList.empty()) {
			parallel::Jobs::ForWorkers(PresentationLanes, [this](size_t begin, size_t end) {
				for (size_t index = begin; index < end; index++) {
					const Presentation &request = PresentationRequests[index];
					PresentationList[index]->Present(request.FrameSeconds, request.Alpha);
				}
			});
		} else {
			for (size_t index = 0; index < PresentationList.size(); index++) {
				const Presentation &request = PresentationRequests[index];
				PresentationList[index]->Present(request.FrameSeconds, request.Alpha);
			}
		}

		return PresentationList.size();
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
