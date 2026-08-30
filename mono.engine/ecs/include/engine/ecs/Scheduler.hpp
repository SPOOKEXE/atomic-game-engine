#pragma once

// Dependency-ordered system execution for one store's tick.
//
// Phases are fixed frame boundaries. Named before/after edges order systems
// inside one phase, and dependency-free read-only systems may run in parallel.
//
// Every system is a profiler span, so the F5 overlay shows where a tick went
// with no extra instrumentation in the systems themselves.
//
// @tier L3 · shared

#include <engine/ecs/Store.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::ecs {

	// A coarse stage in the fixed order of one world update.
	//
	// The declaration order is the phase execution order. Named edges define
	// ordering among systems inside a phase.
	enum class Phase : uint8_t {
		Input,
		Simulation,
		Physics,
		Animation,
		Replication,
		RenderPreparation,
		Render,

		// The number of executable phases; not itself an executable phase.
		Count,

		// Compatibility names for callers split around the former four phases.
		PreSimulation = Input,
		PostSimulation = Physics,
		PreRender = RenderPreparation,
	};

	// Returns the profiler and diagnostic label for a phase.
	//
	// @param phase The phase to name.
	// @return `"?"` for Count or a value outside the declared phases.
	std::string_view GetPhaseName(Phase phase);

	// Named ordering constraints for one system.
	//
	// Edges may only name systems in the same phase. Phase order already defines
	// every cross-phase dependency and cannot be overridden here. Optional edges
	// let reusable module installers order against host-specific systems when
	// those systems are present.
	struct SystemOrder {
		SystemOrder() = default;
		SystemOrder(
			std::vector<std::string> before,
			std::vector<std::string> after,
			std::vector<std::string> beforeIfPresent = {},
			std::vector<std::string> afterIfPresent = {}
		)
			: Before(std::move(before)), After(std::move(after)), BeforeIfPresent(std::move(beforeIfPresent)),
			  AfterIfPresent(std::move(afterIfPresent)) {}

		std::vector<std::string> Before;
		std::vector<std::string> After;
		std::vector<std::string> BeforeIfPresent;
		std::vector<std::string> AfterIfPresent;
	};

	// Result of validating the authored system graph.
	enum class SystemScheduleStatus : uint8_t {
		Ready,
		EmptyName,
		DuplicateName,
		UnknownDependency,
		CrossPhaseDependency,
		Cycle,
	};

	// One actionable schedule validation result.
	struct SystemScheduleIssue {
		SystemScheduleStatus Status = SystemScheduleStatus::Ready;
		std::string System;
		std::string Dependency;
	};

	// Runs named systems in fixed phase order and dependency waves.
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
		// Anything it needs is in the world, so there is nothing to capture -
		// and a system that captures nothing is one the L13 bindings can
		// register, a recording can replay, and a second world can reuse.
		using System = std::function<void(Store &)>;
		using ParallelSystem = std::function<void(const Store &)>;

		// `name` is copied, so a caller may build one. It becomes the span
		// label in the overlay and in Tracy.
		//
		// @param name   The profiler label to copy and retain.
		// @param phase  The phase in which the system runs.
		// @param system The callable to run with the current store.
		// @param order  Named dependencies within the phase.
		void Add(std::string_view name, Phase phase, System system, SystemOrder order = {});

		// Adds a read-only system eligible to share a dependency wave with other
		// read-only systems. Its observable result must not depend on execution
		// order, and captured output must be disjoint or synchronized.
		void AddParallel(std::string_view name, Phase phase, ParallelSystem system, SystemOrder order = {});

		// Replaces one named system without moving it in phase or dependency graph.
		// A strictly increasing revision prevents an older reload from winning.
		//
		// @return `false` when the name is absent, ambiguous, or not newer.
		bool Replace(std::string_view name, uint64_t revision, System system);

		// Replaces one parallel read-only system without changing its edges.
		bool ReplaceParallel(std::string_view name, uint64_t revision, ParallelSystem system);

		// Returns the active revision of one unambiguous named system.
		uint64_t SystemRevision(std::string_view name) const;

		// Reports whether one phase already contains a system with this name.
		bool HasSystem(std::string_view name, Phase phase) const;

		// Validates names, phase-local edges, and acyclicity without running.
		SystemScheduleIssue Validate() const;

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
		// rather than replacing them. Advances no clock - the caller decides
		// when time moves, which is the whole point of splitting the frame.
		//
		// This is what lets simulation and rendering advance at different
		// rates: the simulation phases run zero or more times per frame at the
		// fixed delta, and the render phases run once per frame.
		//
		//     scheduler.ClearTimings();
		//     for (int tick = 0; tick < ticks; tick++) {
		//         store.AdvanceTick(fixed);
		//         scheduler.RunPhases(store, Phase::Input,
		//                             Phase::Replication);
		//     }
		//     store.SetFrame(frameDelta, alpha);
		//     scheduler.RunPhases(store, Phase::RenderPreparation, Phase::Render);
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
		enum class Execution : uint8_t {
			Serial,
			ParallelReadOnly,
		};

		struct Registered {
			// Owned, so that the string_view handed to the profiler outlives
			// the frame that recorded it.
			std::string Name;
			Phase RunPhase = Phase::Simulation;
			System Body;
			ParallelSystem ParallelBody;
			SystemOrder Order;
			uint64_t Revision = 1;
			Execution Mode = Execution::Serial;
		};

		struct Wave {
			std::vector<size_t> Systems;
			bool Parallel = false;
		};

		using Schedule = std::array<std::vector<Wave>, static_cast<size_t>(Phase::Count)>;

		SystemScheduleIssue BuildSchedule(Schedule &schedule) const;
		void EnsureSchedule();
		void RecordTiming(const Registered &system, float milliseconds);

		std::vector<Registered> Systems;
		std::vector<Timing> LastTimings;
		Schedule Compiled;
		bool ScheduleDirty = true;
	};
}
