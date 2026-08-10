#pragma once

// The overarching simulation: every world, and when each one ticks.
//
// A universe owns worlds. It does not own game state — a world's storage is the
// world's — and it never hands a world's store to anybody. `ecs/AGENTS.md` says
// this layer "hands out identifiers, never stores"; that sentence is what this
// class is.
//
// **The driver tick is a barrier and one parallel step:**
//
//     Universe::Tick(frameSeconds)
//       1. Drain the control queue   create / destroy / suspend / recover  [driver]
//       2. Build the active list     worlds owing >= 1 tick, longest first [driver]
//       3. Tick worlds               --------------------------- PARALLEL ---
//       4. Collect diagnostics                                            [driver]
//
// Only step 3 runs on more than one thread, and during it no world touches
// another world or anything the universe holds. Everything structural happens
// at the barrier, on one thread, which is why creating a world from inside a
// tick queues rather than mutating the world list underneath the batch.
//
// **Worlds are the batch, not threads.** Step 3 is one `Jobs::For` over the
// active worlds. There is no thread per world: the pool is sized once, the
// caller drains alongside it, and a world is picked up by whichever worker gets
// to it. That is why a world's store rebinds every tick and why a world tick
// must never block.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/world/Bus.hpp>
#include <engine/world/Enums.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/World.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace engine::world {

	// How a universe spends its host's workers.
	//
	// @since v0.2
	struct UniverseSettings {
		// How the worker pool is spent. See ExecutionMode — the switch changes
		// no result, only where the parallelism is taken.
		ExecutionMode Mode = ExecutionMode::WorldParallel;

		// The largest number of ticks one world may catch up in a single driver
		// tick. Beyond this the excess is dropped and counted, because a world
		// that fell far behind will not recover by trying to run a hundred
		// ticks in one frame — it will only fall further behind.
		int MaximumCatchUpTicks = 8;

		// Whether the buses live in another process.
		//
		// A supervised host holds worlds and nothing else: the MessagingService
		// topics, the MemoryStore map and the DataStore records are the
		// driver's, because two processes each holding a copy would be two
		// answers to the same key. So a federated universe collects what its
		// worlds posted, hands it up the link, and delivers back whatever the
		// driver decided — it never applies an envelope itself.
		//
		// The default is a universe that owns its buses, which is a driver, a
		// single-process server, and a client. A host says so explicitly.
		bool Federated = false;

		// How many bus operations one world may issue per tick.
		//
		// A world in a loop would otherwise fill the barrier with its own
		// traffic and starve every other world in the universe. Roblox has
		// request budgets because they turned out to be necessary; there is no
		// reason to rediscover that.
		uint32_t BusBudgetPerTick = 64;
	};

	// One delivery for a world that lives in another process.
	//
	// Carries the host as well as the world, because the driver's directory is
	// the only thing that knows which host holds which world and the link does
	// not — a host is told about its own worlds and nothing else.
	//
	// @since v0.2
	struct RemoteDelivery {
		// The host holding the destination.
		core::Name Host;

		// The world the delivery is for.
		core::Name World;

		// What arrived for it.
		Delivery Message;
	};

	// Diagnostics for one driver tick.
	//
	// @since v0.2
	struct UniverseStatistics {
		// Worlds that ran at least one tick in the most recent driver tick.
		size_t ActiveWorlds = 0;

		// Worlds currently suspended — not ticking, and expected back.
		size_t Suspended = 0;

		// Worlds held by a supervised host rather than by this process.
		size_t Remote = 0;

		// Worlds currently quarantined by a soft fault. Distinct from
		// Suspended: one is a decision and the other is a symptom, and a count
		// that climbs here is the crash loop the cap exists to bound.
		size_t Faulted = 0;

		// Wall milliseconds the most recent driver tick took, end to end.
		float LastTickMilliseconds = 0.0f;

		// Simulation ticks run across every world in the most recent driver
		// tick. The number that says whether the universe is keeping up.
		uint64_t SimulationTicks = 0;

		// Bus operations applied at the most recent barrier.
		uint64_t BusOperations = 0;

		// Deliveries handed to worlds at the most recent barrier.
		uint64_t Deliveries = 0;
	};

	// Owns worlds and drives them.
	//
	// @since v0.2
	class Universe {
	  public:
		// Creates an empty universe bound to the calling thread.
		//
		// That thread is the driver: the only one allowed to create, destroy or
		// enter a world.
		//
		// @param settings How this universe spends its workers.
		explicit Universe(const UniverseSettings &settings = {});

		// Destroys every world.
		~Universe();

		// A universe owns worlds and is never copied.
		Universe(const Universe &) = delete;

		// A universe owns worlds and is never copy-assigned.
		Universe &operator=(const Universe &) = delete;

		// Creates a world, or returns the one already holding that name.
		//
		// Applied immediately when called from the driver thread outside a
		// tick, and queued to the next barrier when called from inside one —
		// because a world list that grew underneath a running batch would move
		// the very worlds the batch is iterating.
		//
		// @param settings What the world is created with.
		// @param status   Set to why, when the handle comes back invalid.
		// @return The world's handle, or an invalid handle.
		WorldId Create(const WorldSettings &settings, WorldStatus *status = nullptr);

		// Registers a world held by a supervised host.
		//
		// The driver keeps a record of it — a name, a state of `Remote`, and
		// the host holding it — so that everything a world is addressed *by*
		// keeps working: a topic it subscribed to, a teleport sent to it, a
		// reply owed to it. What the driver does not have is its storage, so
		// the world is never ticked here and its store is never read.
		//
		// This is what lets one barrier hold local and remote worlds together.
		// Without it the driver would need a second routing path, and two
		// routing paths agree until the day one of them does not.
		//
		// @param settings What the world was created with, in its host.
		// @param host     The host holding it.
		// @param status   Set to why, when the handle comes back invalid.
		// @return The world's handle, or an invalid handle.
		WorldId CreateRemote(const WorldSettings &settings, core::Name host, WorldStatus *status = nullptr);

		// The host holding a world.
		//
		// @param id The world to ask about.
		// @return The host's name, or an invalid Name for a local or unknown
		//         world.
		core::Name HostOf(WorldId id) const;

		// Whether a world lives in another process.
		//
		// @param id The world to ask about.
		// @return `true` when it is held by a host.
		bool IsRemote(WorldId id) const;

		// Destroys a world and everything in it.
		//
		// Queued to the barrier when called from inside a tick.
		//
		// @param id The world to destroy.
		// @return Whether it was destroyed or queued for destruction.
		WorldStatus Destroy(WorldId id);

		// The world registered under a name.
		//
		// The lookup everything crossing a boundary uses, because a name is
		// what crosses and an index is not.
		//
		// @param name The world's stable name.
		// @return The handle, or an invalid handle.
		WorldId Find(core::Name name) const;

		// The number of worlds this universe holds.
		//
		// @return The world count, including suspended and faulted ones.
		size_t Count() const;

		// The number of worlds in one state.
		//
		// **Exists because `Count` answers the wrong question for a lifecycle
		// decision.** `world::DecideLifecycle` refuses to suspend the last
		// world, and a host feeding that refusal a total — which includes the
		// worlds already suspended — suspends a whole universe one world at a
		// time, each of them the last only after the rest had gone. Counting
		// what is still `Active` is the fact that refusal is about, and it lives
		// here so both hosts cannot answer it differently.
		//
		// @param state The state to count.
		// @return How many worlds are in it.
		// @since v0.13
		size_t CountInState(WorldState state) const;

		// Every world's handle, in creation order.
		//
		// @return The handles, copied.
		std::vector<WorldId> Worlds() const;

		// What a world is called.
		//
		// @param id The world to ask about.
		// @return The name, or an invalid Name for an unknown world.
		core::Name NameOf(WorldId id) const;

		// What a world was configured with.
		//
		// **Read-only, and there is no setter to match it.** A world's tick
		// rate is decided when it is created and changing one underneath a
		// running simulation is a different operation with different answers
		// about the ticks already in flight. This exists because a save file
		// has to write what a world actually is: `game::WriteGame` wrote the
		// defaults for every world before it, so a scene authored at 30Hz
		// saved as 60 and nothing said so.
		//
		// @param id The world to ask about.
		// @return The settings, or a default-constructed set for an unknown
		//         world.
		WorldSettings SettingsOf(WorldId id) const;

		// What state a world is in.
		//
		// @param id The world to ask about.
		// @return The state, or Suspended for an unknown world.
		WorldState StateOf(WorldId id) const;

		// How far a world's next frame sits between its last two ticks.
		//
		// @param id The world to ask about.
		// @return The interpolation position, or zero for an unknown world.
		float AlphaOf(WorldId id) const;

		// One world's diagnostics.
		//
		// @param id The world to ask about.
		// @return The statistics, or an empty record for an unknown world.
		WorldStatistics StatisticsOf(WorldId id) const;

		// Moves a world between Active, Idle and Suspended.
		//
		// Queued to the barrier when called from inside a tick.
		//
		// @param id    The world to move.
		// @param state The state to move it to.
		// @return Whether it was applied or queued.
		WorldStatus SetState(WorldId id, WorldState state);

		// Clears a world's fault so it ticks again.
		//
		// @param id The world to recover.
		// @return `NoSuchWorld`, or `Ok` even when the world is being held down
		//         for faulting too often — the caller reads `StateOf` to see.
		WorldStatus Recover(WorldId id);

		// Runs one driver tick: the barrier, then the worlds.
		//
		// @param frameSeconds Wall seconds since the previous driver tick.
		// @tick
		void Tick(float frameSeconds);

		// Runs one world's presentation phase, on the driver thread.
		//
		// Separate from Tick because a client renders one world while the rest
		// keep simulating, and because presentation advances at the frame rate
		// rather than the tick rate.
		//
		// @param id           The world to present.
		// @param frameSeconds Wall seconds the frame took.
		// @param alpha        Interpolation position between the last two ticks.
		// @return Whether the world exists.
		WorldStatus Present(WorldId id, float frameSeconds, float alpha);

		// Runs `body` against a world's storage, on the driver thread.
		//
		// **The only way to reach a store, and deliberately a scoped one.** A
		// long-lived `Store &` is what makes thread-per-world and
		// process-per-world different designs; a reference that exists for the
		// length of a call does not. When a world lives in another process this
		// becomes a request, and nothing above this line changes.
		//
		// Aborts if called while a tick batch is in flight, because the world
		// may be mid-tick on a worker and the affinity check would be the only
		// thing standing between that and a data race.
		//
		// @param id   The world to enter.
		// @param body Called as `body(Store &, Scheduler &)`.
		// @return Whether the world exists.
		WorldStatus Enter(WorldId id, const std::function<void(ecs::Store &, ecs::Scheduler &)> &body);

		// Runs `body` against a world's storage, without the scheduler.
		//
		// The same door as the overload above — there is deliberately no
		// read-only variant. `Store::Each` is not const, because iterating
		// caches a query plan, so a `const Store &` could not be iterated and a
		// view that cannot be walked is not a view. A caller that only reads
		// takes this overload and reads.
		//
		// @param id   The world to enter.
		// @param body Called as `body(Store &)`.
		// @return Whether the world exists.
		WorldStatus Enter(WorldId id, const std::function<void(ecs::Store &)> &body);

		// Diagnostics for the most recent driver tick.
		//
		// @return The statistics, copied.
		UniverseStatistics Statistics() const {
			return Stats;
		}

		// How this universe spends its workers.
		//
		// @return The settings.
		const UniverseSettings &Settings() const {
			return Settings_;
		}

		// Changes the execution mode.
		//
		// A tuning knob rather than a semantic: the switch changes where
		// parallelism is taken and not what is computed.
		//
		// @param mode The mode to run in.
		void SetMode(ExecutionMode mode);

		// The number of worlds subscribed to a topic.
		//
		// A diagnostic rather than a routing primitive: nothing needs this to
		// deliver a message, and a world asking who is listening would be a
		// world learning about other worlds.
		//
		// @param topic The topic to count.
		// @return The subscriber count.
		size_t SubscriberCount(core::Name topic) const;

		// Reads a bus value directly, on the driver thread.
		//
		// The escape hatch `v02v03.md` §2.8 describes: for persistence, an
		// admin console, a stats scrape — consumers genuinely outside the
		// simulation. **A system may never call this.** Anything read through
		// it is non-replayable by definition, so nothing the simulation depends
		// on may live behind it.
		//
		// @param bus   The bus to read.
		// @param key   The key to read.
		// @param value Filled with the value when one is present.
		// @return `Ok`, or `NotFound`.
		BusStatus Peek(BusKind bus, core::Name key, std::vector<std::byte> *value) const;

		// --- snapshots and replay ------------------------------------------

		// Writes every world, every bus, and the settings behind them.
		//
		// Worlds are recorded by **name**, and so are bus subscribers and keys,
		// so a universe written by one process restores into another that
		// numbered its worlds differently. That is what a crash restart and a
		// recording replayed by a later build both need.
		//
		// @param writer The writer to append to.
		// @return `false` when a world holds a component with no serialisation.
		bool Save(core::ByteWriter &writer) const;

		// Replaces everything with a saved universe.
		//
		// On any failure the universe is left **empty** rather than half
		// restored, for the same reason a store is: a universe that is partly
		// one snapshot and partly another looks like it works.
		//
		// @param reader The reader to consume.
		// @return `false` on a corrupt, truncated or wrong-version snapshot.
		bool Load(core::ByteReader &reader);

		// The bus traffic applied at the most recent barrier.
		//
		// This is what a recording records. Stamped with `From` and ordered
		// exactly as it was applied, so replaying it reproduces the barrier
		// rather than approximating it.
		//
		// @return A view valid until the next Tick.
		std::span<const Envelope> LastTraffic() const;

		// Uses `traffic` for the next barrier instead of the worlds' outboxes.
		//
		// The replay path. A world's own outbox is discarded for that barrier —
		// a replayed world re-derives the same requests, and applying both the
		// recorded and the re-derived copy would double every operation.
		//
		// @param traffic The recorded envelopes, in the order they were applied.
		void InjectTraffic(std::vector<Envelope> traffic);

		// Queues one delivery for a world's next barrier.
		//
		// What a federated universe does with what its driver sends back. Not
		// for a system: a world posts through `Postbox` and receives through
		// its inbox, and a caller reaching in here is the driver's side of a
		// link rather than anything inside the simulation.
		//
		// @param world    The world to deliver to.
		// @param delivery What arrived for it.
		// @return `false` for an unknown world.
		bool Deliver(core::Name world, const Delivery &delivery);

		// Hands the driver what a host's worlds posted.
		//
		// Applied at the next barrier alongside what local worlds posted, in
		// the same `(From, Sequence)` order, so a universe split across
		// processes routes identically to one that is not.
		//
		// **An envelope whose sender is not one of that host's worlds is
		// dropped.** This is `Envelope::From` being stamped rather than trusted,
		// carried across a process boundary: a host that could claim to be a
		// world it does not hold could read that world's replies and publish in
		// its name.
		//
		// @param host    The host that sent them.
		// @param traffic The envelopes it collected.
		// @return The number accepted. Anything less means some were refused.
		size_t IngestTraffic(core::Name host, std::span<const Envelope> traffic);

		// What the last barrier decided for worlds that live elsewhere.
		//
		// The driver hands these to the hosts holding them. Accumulated rather
		// than replaced, so a driver that services its links less often than it
		// ticks does not lose deliveries — it is the caller's job to take them.
		//
		// @return The pending deliveries, in barrier order.
		std::span<const RemoteDelivery> Outbound() const;

		// Takes the pending remote deliveries, leaving none.
		//
		// @return The deliveries, in barrier order.
		std::vector<RemoteDelivery> TakeOutbound();

		// The snapshot format this build writes and accepts.
		static constexpr uint32_t SNAPSHOT_VERSION = 2;

		// Reports whether the caller is the driver thread.
		//
		// @return `true` when structural operations are allowed.
		// @threadsafe
		bool IsOnDriverThread() const;

	  private:
		// A structural change waiting for the barrier.
		struct Control {
			enum class Kind : uint8_t { Create, Destroy, SetState, Recover };

			Kind What = Kind::Create;
			WorldId Target;
			WorldSettings Settings;
			WorldState State = WorldState::Active;

			// The host, for a `Create` that is registering a remote world.
			core::Name Host;
		};

		void Apply(const Control &control);
		void DrainControls();
		WorldId Adopt(const WorldSettings &settings, core::Name host = {});
		World *Reach(WorldId id);
		const World *Reach(WorldId id) const;
		void RequireDriverThread(const char *what) const;

		UniverseSettings Settings_;
		UniverseStatistics Stats;

		// Held by pointer so that a reference to one survives the list growing,
		// and so that destroying a world does not move its neighbours.
		std::vector<std::unique_ptr<World>> Registry;

		// The host holding each world, parallel to `Registry`. An invalid Name
		// is a world this process holds.
		//
		// Parallel rather than a field on `World`, because a `World` is storage
		// plus a scheduler plus a clock and a remote one has none of the three
		// — putting the host on it would be putting a driver's bookkeeping
		// inside the thing being booked.
		std::vector<core::Name> Hosts;

		std::vector<Control> Pending;

		// Reused between driver ticks rather than rebuilt, because the active
		// list is walked every frame and most frames it is nearly the same.
		std::vector<World *> ActiveList;
		std::vector<int> OwedList;

		// The bus backends and the barrier that applies traffic to them. Held
		// by pointer so the header does not have to describe either — a
		// DataStore's table is nobody else's business, and neither is the order
		// the driver puts a tick's envelopes in.
		std::unique_ptr<class BusRouter> Router;

		std::thread::id Driver;
		bool Ticking = false;
	};
}
