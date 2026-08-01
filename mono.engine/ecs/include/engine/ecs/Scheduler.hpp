#pragma once

// Ordered system execution for one store's tick.
//
// Phases exist so that ordering is a property of what a system *is* rather than
// of when somebody happened to register it. Two systems in the same phase have
// no ordering guarantee relative to each other, and that is deliberate: if the
// order matters, they are in the wrong phases.
//
// Every system is a profiler span, so the F5 overlay shows where a tick went
// with no extra instrumentation in the systems themselves.
//
// @tier L3 · shared

#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {

	// A coarse stage in the fixed order of one world update.
	//
	// The declaration order is the execution order. Systems sharing a phase have
	// no ordering contract relative to one another.
	enum class Phase : uint8_t {
		// Input, timers, and anything that decides what this tick is about.
		PreSimulation,
		// The tick proper.
		Simulation,
		// Consequences: collisions resolved, lifetimes expired.
		PostSimulation,
		// Deriving what to draw. Never mutates simulation state.
		PreRender,

		// The number of executable phases; not itself an executable phase.
		Count,
	};

	// Returns the profiler and diagnostic label for a phase.
	//
	// @param phase The phase to name.
	// @return `"?"` for Count or a value outside the declared phases.
	std::string_view GetPhaseName(Phase phase);

	// Runs named systems against a Store in phase order and records their cost.
	//
	// Phase order is the only ordering contract. Registration order must not be
	// used to express a dependency between systems in the same phase.
	class Scheduler {
	  public:
		// A system takes the world and nothing else.
		//
		// Not `void(Store &, float)`: a delta handed in from outside is a delta
		// that can be the wrong one, and "this system was passed the frame time
		// instead of the tick time" does not look like a bug at the call site.
		// A system reads `store.Time()`, where the two are separate fields with
		// separate names.
		//
		// The signature is also the reason a system can be a plain function.
		// Anything it needs is in the world, so there is nothing to capture —
		// and a system that captures nothing is one the L13 bindings can
		// register, a recording can replay, and a second world can reuse.
		using System = std::function<void(Store &)>;

		// `name` is copied, so a caller may build one. It becomes the span
		// label in the overlay and in Tracy.
		//
		// Adding systems to the same phase does not establish a supported order
		// between them.
		//
		// @param name   The profiler label to copy and retain.
		// @param phase  The phase in which the system runs.
		// @param system The callable to run with the current store.
		void Add(std::string_view name, Phase phase, System system);

		// One whole tick: advance the world's clock by `deltaSeconds`, then run
		// every phase in order on the calling thread. The store's affinity
		// check makes running this on the wrong thread an abort rather than a
		// race.
		//
		// One rate for everything, which is what a server is. A client drives
		// the clock itself and calls RunPhases, because its simulation and its
		// rendering do not advance together.
		//
		// @param store        The world storage to advance and run.
		// @param deltaSeconds Simulated seconds advanced before systems run.
		// @tick
		void Tick(Store &store, float deltaSeconds);

		// Runs `first` through `last` inclusive, accumulating into the timings
		// rather than replacing them. Advances no clock — the caller decides
		// when time moves, which is the whole point of splitting the frame.
		//
		// This is what lets simulation and rendering advance at different
		// rates: the simulation phases run zero or more times per frame at the
		// fixed delta, and the render phases run once per frame.
		//
		//     scheduler.ClearTimings();
		//     for (int tick = 0; tick < ticks; tick++) {
		//         store.AdvanceTick(fixed);
		//         scheduler.RunPhases(store, Phase::PreSimulation,
		//                             Phase::PostSimulation);
		//     }
		//     store.SetFrame(frameDelta, alpha);
		//     scheduler.RunPhases(store, Phase::PreRender, Phase::PreRender);
		//
		// @param store The world storage handed to each matching system.
		// @param first The first phase to run.
		// @param last  The last phase to run, inclusive.
		void RunPhases(Store &store, Phase first, Phase last);

		// Starts a frame's timings. Tick does this for you; a caller splitting
		// the frame across several RunPhases calls has to do it once itself, or
		// timings from earlier frames remain in the overlay totals.
		void ClearTimings();

		// The accumulated cost of one named system in the current timing frame.
		struct Timing {
			// The registered system name, owned by this Scheduler.
			std::string_view Name;

			// The phase in which the system ran.
			Phase RunPhase = Phase::Simulation;

			// Total wall-clock milliseconds spent in the system since timings cleared.
			float Milliseconds = 0.0f;
		};

		// Returns accumulated per-system costs since ClearTimings.
		//
		// The returned reference and its name views remain valid only until the
		// next non-const operation on this Scheduler.
		//
		// @return Timing rows for systems that have run since the last clear.
		const std::vector<Timing> &Timings() const {
			return LastTimings;
		}

		// Returns the number of registered systems.
		//
		// @return The current registration count across all phases.
		size_t SystemCount() const {
			return Systems.size();
		}

	  private:
		struct Registered {
			// Owned, so that the string_view handed to the profiler outlives
			// the frame that recorded it.
			std::string Name;
			Phase RunPhase = Phase::Simulation;
			System Body;
		};

		std::vector<Registered> Systems;
		std::vector<Timing> LastTimings;
	};
}
