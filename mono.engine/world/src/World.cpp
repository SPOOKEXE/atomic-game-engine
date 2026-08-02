#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/world/World.hpp>

#include <algorithm>
#include <exception>

namespace engine::world {

	World::World(WorldId id, const WorldSettings &settings)
		: Handle(id), Settings_(settings), Store_(settings.Name.Text()), Timestep(settings.TickRate) {
		// The root exists from the start, so no system has to check whether the
		// world has one. It is this world's `workspace`.
		RootEntity = Store_.Create("workspace");
	}

	int World::Owed(float frameSeconds) {
		if (State_ == WorldState::Suspended || State_ == WorldState::Faulted ||
			State_ == WorldState::Remote) {
			// Not merely skipped: the accumulator is not advanced either, so a
			// world resumed after an hour does not owe a hour of ticks.
			return 0;
		}

		Timestep.SetRate(State_ == WorldState::Idle ? Settings_.IdleTickRate : Settings_.TickRate);

		const int owed = Timestep.Advance(frameSeconds);

		// The timestep gives up on a stall rather than trying to catch it, and
		// the count it dropped is worth surfacing: a figure that climbs is a
		// world that cannot keep up with its own rate.
		Stats.DroppedTicks = Timestep.Dropped();
		return owed;
	}

	void World::Tick(int ticks) {
		if (ticks <= 0) {
			return;
		}

		ENGINE_PROFILE_CAT("World::Tick", engine::core::ProfileCategory::Simulation);

		// Whichever worker claimed this world runs it, so the handoff is the
		// ordinary case rather than a setup step.
		Store_.BindToCallingThread();

		const uint64_t started = core::Clock::Nanoseconds();

		try {
			for (int tick = 0; tick < ticks; tick++) {
				// Cleared at the *start* of a tick rather than the end, so
				// what a tick recorded is still there for `Present` to read —
				// render invalidation runs in `PreRender`, which is a separate
				// call after this one. Clearing at the end would hand the
				// renderer an empty set every frame.
				Store_.ClearChanges();

				Store_.AdvanceTick(Timestep.Delta());
				Scheduler_.RunPhases(Store_, ecs::Phase::PreSimulation, ecs::Phase::PostSimulation);

				// The phase boundary the change signals are named for. After
				// the simulation phases, so a property written three times in
				// one tick signals once with the value it ended up at — and
				// never inside a batch, where a listener could mutate the world
				// in the middle of a loop over it.
				Store_.FlushSignals();
			}
			ConsecutiveFaults = 0;
		} catch (const std::exception &failure) {
			// A soft fault: one world stops, its neighbours never notice. A
			// hard fault is not caught and cannot be — see AGENTS.md.
			Stats.Faults++;
			ConsecutiveFaults++;
			State_ = WorldState::Faulted;

			ENGINE_ERROR(
				"world '{}' faulted on tick {}: {}", Settings_.Name.Text(), Store_.Time().Tick, failure.what()
			);
		} catch (...) {
			Stats.Faults++;
			ConsecutiveFaults++;
			State_ = WorldState::Faulted;

			ENGINE_ERROR("world '{}' faulted on tick {}", Settings_.Name.Text(), Store_.Time().Tick);
		}

		const auto elapsed =
			static_cast<float>(static_cast<double>(core::Clock::Nanoseconds() - started) / 1'000'000.0);
		Stats.LastTickMilliseconds = elapsed;
		Stats.SlowestTickMilliseconds = std::max(Stats.SlowestTickMilliseconds, elapsed);
	}

	void World::Present(float frameSeconds, float alpha) {
		if (State_ == WorldState::Faulted) {
			return;
		}

		Store_.BindToCallingThread();
		Store_.SetFrame(frameSeconds, alpha);
		Scheduler_.RunPhases(Store_, ecs::Phase::PreRender, ecs::Phase::PreRender);
	}

	void World::SetState(WorldState state) {
		if (State_ == WorldState::Faulted && state != WorldState::Faulted) {
			// Leaving a fault goes through Recover, which is where the
			// crash-loop cap lives. Otherwise a caller could clear a fault by
			// setting the state and the cap would never fire.
			return;
		}
		State_ = state;
	}

	bool World::Recover() {
		if (State_ != WorldState::Faulted) {
			return true;
		}

		if (ConsecutiveFaults >= Settings_.FaultLimit) {
			// Held down deliberately. A world that faults every tick would
			// otherwise restore and re-fault forever, burning its host's budget
			// while looking alive on every dashboard.
			ENGINE_WARN(
				"world '{}' has faulted {} times in a row and is being held down.",
				Settings_.Name.Text(),
				ConsecutiveFaults
			);
			return false;
		}

		State_ = WorldState::Active;
		return true;
	}
}
