#include <engine/core/Clock.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Scheduler.hpp>

#include <algorithm>

namespace engine::ecs {

	std::string_view GetPhaseName(Phase phase) {
		switch (phase) {
		case Phase::PreSimulation:
			return "pre-simulation";
		case Phase::Simulation:
			return "simulation";
		case Phase::PostSimulation:
			return "post-simulation";
		case Phase::PreRender:
			return "pre-render";
		case Phase::Count:
			break;
		}
		return "?";
	}

	void Scheduler::Add(std::string_view name, Phase phase, System system) {
		Systems.push_back(Registered{std::string(name), phase, std::move(system)});

		// Registered systems are stable for the life of the scheduler, so the
		// timing list is sized once and reused. Reallocating it inside Run
		// would show up in the numbers it is reporting.
		LastTimings.reserve(Systems.size());
	}

	void Scheduler::ClearTimings() {
		LastTimings.clear();
	}

	void Scheduler::Tick(Store &store, float deltaSeconds) {
		// The clock moves first, so every system in the tick — including the
		// ones in PreSimulation — sees the time it is simulating rather than
		// the time it has just left.
		store.AdvanceTick(deltaSeconds);

		ClearTimings();
		RunPhases(store, Phase::PreSimulation, Phase::PreRender);
	}

	void Scheduler::RunPhases(Store &store, Phase first, Phase last) {
		// `ECS`, not `Simulation`. Every engine and game system runs through
		// here, so this is where a game's time goes — and a category that only
		// said "simulation" could not separate the systems from the driver
		// around them.
		ENGINE_PROFILE_CAT("ecs.systems", core::ProfileCategory::ECS);

		for (uint8_t index = static_cast<uint8_t>(first);
			 index <= static_cast<uint8_t>(last) && index < static_cast<uint8_t>(Phase::Count);
			 index++) {
			const auto phase = static_cast<Phase>(index);

			// A span per phase, between the scheduler and its systems.
			//
			// **This is the level a bottleneck is obvious at.** One bar for the
			// whole tick says the tick is slow; forty bars, one per system, is
			// a wall of text nobody reads at sixty frames a second. Four bars
			// — one per phase — is the shape that answers "which part" in a
			// glance, and the systems under it answer "which one" when you
			// look closer.
			//
			// STABLE, because `GetPhaseName` returns a literal that outlives
			// the frame; there is nothing to copy.
			const std::string_view phaseName = GetPhaseName(phase);
			ENGINE_PROFILE_DYNAMIC_STABLE("phase", phaseName, core::ProfileCategory::ECS);

			for (auto &system : Systems) {
				if (system.RunPhase != phase) {
					continue;
				}

				const uint64_t started = core::Clock::Nanoseconds();
				{
					// STABLE, not the copying form: the scheduler owns the
					// string for the life of the run, so there is nothing to
					// copy and no name to keep alive.
					const std::string_view label = system.Name;
					ENGINE_PROFILE_DYNAMIC_STABLE("system", label, core::ProfileCategory::ECS);
					system.Body(store);
				}
				const uint64_t finished = core::Clock::Nanoseconds();

				// Accumulated, not appended, because a simulation system runs
				// more than once in a frame that owed several ticks. What the
				// overlay wants is what that system cost *this frame*, not the
				// last of three identical rows.
				const auto milliseconds =
					static_cast<float>(static_cast<double>(finished - started) / 1'000'000.0);

				auto existing =
					std::find_if(LastTimings.begin(), LastTimings.end(), [&system](const Timing &timing) {
						return timing.Name == system.Name;
					});
				if (existing != LastTimings.end()) {
					existing->Milliseconds += milliseconds;
				} else {
					LastTimings.push_back(Timing{system.Name, phase, milliseconds});
				}
			}
		}
	}
}
