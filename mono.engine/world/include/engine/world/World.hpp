#pragma once

// One world: a store, the systems that run over it, and its own clock.
//
// A world is a *subarea to simulate* - one zone, one instanced dungeon, one
// lobby. It owns everything its tick reads and writes, and it can reach nothing
// outside itself: no other world, no shared mutable state, no global. That is
// what lets a hundred of them tick at once with no locks, and what makes moving
// one to another process a change of transport rather than a change of design.
//
// **Its own rate.** Worlds do not agree on a tick rate and do not have to. A
// busy zone runs at 60 Hz, a dormant one at 2, and a suspended one at none.
// A universe of many subareas pays for the ones somebody is in.
//
// Owned by `Universe`. Nothing constructs one directly, because a world that
// existed outside a universe would have nowhere to send anything.
//
// @tier L4 · shared

#include <engine/core/FixedTimestep.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/world/Enums.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace engine::world {

	// A process-local handle for one world.
	//
	// Dense and never serialised: the stable identity is the world's
	// `core::Name`, which is what a save file, a bus envelope and a supervisor
	// all carry. An index is meaningless in any of those.
	//
	// @since v0.2
	struct WorldId {
		// The value no world is given.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The index into the universe's world list.
		uint32_t Index = INVALID;

		// Creates an invalid handle.
		constexpr WorldId() = default;

		// Creates a handle from an index.
		//
		// @param index The index to wrap.
		constexpr explicit WorldId(uint32_t index) : Index(index) {}

		// Reports whether this handle names a world.
		//
		// @return `true` when the handle came from a universe.
		constexpr bool IsValid() const {
			return Index != INVALID;
		}

		// Compares handles for equality.
		//
		// @param other The handle to compare.
		// @return `true` when both name the same world.
		constexpr bool operator==(const WorldId &other) const {
			return Index == other.Index;
		}

		// Compares handles for inequality.
		//
		// @param other The handle to compare.
		// @return `true` when the handles name different worlds.
		constexpr bool operator!=(const WorldId &other) const {
			return Index != other.Index;
		}
	};

	// What a world is created with.
	//
	// @since v0.2
	struct WorldSettings {
		// The stable name. Required - everything that crosses a boundary
		// addresses a world by name, so a world without one cannot be reached.
		core::Name Name;

		// Simulation ticks per second while Active.
		double TickRate = 60.0;

		// Ticks per second while Idle.
		//
		// Not zero: a dormant subarea still has to advance, because crops grow
		// and timers expire whether or not somebody is watching.
		double IdleTickRate = 2.0;

		// Whether this world can tolerate sharing a process.
		//
		// Not spelled `Isolation`: a member may not share a name with its own
		// type, and the compiler is right to refuse it.
		enum Isolation IsolationLevel = Isolation::Shared;

		// How many consecutive faults before the world is held down.
		//
		// A world that faults deterministically every tick would otherwise
		// restore and re-fault forever, consuming its host's budget while
		// looking alive on every dashboard.
		uint32_t FaultLimit = 3;

		// The universe rendering profile this world presents through.
		//
		// A name rather than a graph because `world` sits below `graph`, and
		// because the selection crosses game files and host boundaries. The
		// client resolves it against the universe's authored profile library.
		core::Name RenderingProfile{"Default PBR"};
	};

	// One world's diagnostics, as the driver and the overlay read them.
	//
	// @since v0.2
	struct WorldStatistics {
		// Simulation ticks run since the world was created.
		//
		// The world's clock is the authority; this is a copy of it taken when
		// the statistics were read.
		uint64_t Ticks = 0;

		// Wall milliseconds the most recent tick took.
		float LastTickMilliseconds = 0.0f;

		// The slowest tick since the world was created.
		float SlowestTickMilliseconds = 0.0f;

		// How many times this world's tick has thrown.
		uint32_t Faults = 0;

		// Ticks the world owed and did not get, because it exceeded the
		// catch-up cap. A number that keeps climbing is a world that cannot
		// keep up with its own rate.
		uint64_t DroppedTicks = 0;
	};

	// A store, a scheduler, a clock, and the state machine around them.
	//
	// @since v0.2
	class World {
	  public:
		// Creates a world and its storage.
		//
		// @param id       The handle the universe issued.
		// @param settings What the world was created with.
		World(WorldId id, const WorldSettings &settings);

		// A world owns a store and is never copied.
		World(const World &) = delete;

		// A world owns a store and is never copy-assigned.
		World &operator=(const World &) = delete;

		// The handle this world was issued.
		//
		// @return The process-local id.
		WorldId Id() const {
			return Handle;
		}

		// The stable name, which is what crosses every boundary.
		//
		// @return The world's name.
		core::Name Name() const {
			return Settings_.Name;
		}

		// What the world was created with.
		//
		// @return The settings, including the rates and the isolation level.
		const WorldSettings &Settings() const {
			return Settings_;
		}

		// Changes which universe rendering profile presents this world.
		//
		// This does not change simulation timing, so unlike the tick settings it
		// is safe to update between frames.
		void SetRenderingProfile(core::Name profile) {
			Settings_.RenderingProfile = profile;
		}

		// Whether the world is ticking, and why not when it is not.
		//
		// @return The current state.
		WorldState State() const {
			return State_;
		}

		// Diagnostics since creation.
		//
		// The tick count is read from the world's own clock rather than
		// counted here. A second copy of it was exactly the "two copies of one
		// fact" `ecs/AGENTS.md` forbids, and it drifted the first time it
		// mattered: a world restored from a snapshot came back with the store's
		// tick count and this one's zero.
		//
		// @return The statistics, copied.
		WorldStatistics Statistics() const {
			WorldStatistics copy = Stats;
			copy.Ticks = Store_.Time().Tick;
			return copy;
		}

		// The world's root instance - its `workspace`.
		//
		// Created with the world, so a system never has to check whether the
		// world has one.
		//
		// @return The root entity.
		ecs::Entity Root() const {
			return RootEntity;
		}

		// How far the frame being drawn sits between the last tick and the next.
		//
		// The world owns its own accumulator, so this is the world's answer
		// rather than one the caller kept beside it - which is the same reason
		// the tick count comes from the clock.
		//
		// @return The interpolation position, 0..1.
		float Alpha() const {
			return Timestep.Alpha();
		}

		// Advances the world's accumulator and reports how many ticks it owes.
		//
		// Called on the driver thread, before the parallel batch, because the
		// driver has to know which worlds are worth handing to a worker.
		//
		// @param frameSeconds Wall seconds since the previous driver tick.
		// @return The number of simulation ticks owed, possibly zero.
		int Owed(float frameSeconds);

		// Runs the owed simulation ticks.
		//
		// Called from a job worker, so it binds the store to the calling thread
		// first - which is the ordinary case rather than a handoff, since
		// whichever worker claims the world runs it.
		//
		// A system that throws is caught here and marks the world `Faulted`;
		// the world stops ticking and its neighbours never notice. A hard fault
		// is not caught, and cannot be - see AGENTS.md.
		//
		// @param ticks The number of ticks to run, from Owed.
		// @tick
		void Tick(int ticks);

		// Runs the presentation phase once, at the driver's frame rate.
		//
		// Separate from Tick because simulation and rendering do not advance
		// together: the simulation runs zero or more times per frame at a fixed
		// delta, and this runs once with whatever is left over as `alpha`.
		//
		// @param frameSeconds Wall seconds the frame took.
		// @param alpha        Interpolation position between the last two ticks.
		void Present(float frameSeconds, float alpha);

		// Moves the world between Active, Idle and Suspended.
		//
		// Applied by the universe at the barrier rather than immediately, so
		// nothing changes state while a batch is in flight.
		//
		// @param state The state to move to.
		void SetState(WorldState state);

		// Clears a fault and lets the world tick again.
		//
		// @return `false` when the world has faulted too many times and is
		//         being held down deliberately.
		bool Recover();

		// The storage. **Private to this module on purpose** - see AGENTS.md.
		//
		// `Universe::Enter` is how anything else reaches a world's store, and
		// it takes the reference away again when it returns.
		//
		// @return The world's storage.
		ecs::Store &Storage() {
			return Store_;
		}

		// The systems that run over this world.
		//
		// @return The world's scheduler.
		ecs::Scheduler &Systems() {
			return Scheduler_;
		}

	  private:
		WorldId Handle;
		WorldSettings Settings_;
		WorldState State_ = WorldState::Active;

		ecs::Store Store_;
		ecs::Scheduler Scheduler_;
		core::FixedTimestep Timestep;

		ecs::Entity RootEntity;
		WorldStatistics Stats;
		uint32_t ConsecutiveFaults = 0;
	};
}
