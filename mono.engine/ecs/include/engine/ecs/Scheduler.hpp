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
#include <memory>
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
		// Creates an ordering declaration from required and optional edges.
		SystemOrder() = default;
		// Creates an ordering declaration from required and optional edges.
		SystemOrder(
			std::vector<std::string> before,
			std::vector<std::string> after,
			std::vector<std::string> beforeIfPresent = {},
			std::vector<std::string> afterIfPresent = {}
		)
			: Before(std::move(before)), After(std::move(after)), BeforeIfPresent(std::move(beforeIfPresent)),
			  AfterIfPresent(std::move(afterIfPresent)) {}

		// Required systems that must run after and before this system, respectively.
		//@{
		std::vector<std::string> Before;
		std::vector<std::string> After;
		//@}

		// Optional counterparts to `Before` and `After`.
		//@{
		std::vector<std::string> BeforeIfPresent;
		std::vector<std::string> AfterIfPresent;
		//@}
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
		// The validation result and the edge that caused it.
		//@{
		SystemScheduleStatus Status = SystemScheduleStatus::Ready;
		std::string System;
		std::string Dependency;
		//@}
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
		// A read-only system eligible to share a dependency wave.
		using ParallelSystem = std::function<void(const Store &)>;

		Scheduler() = default;
		// Copies registrations and rebinds timing-name views to the copied names.
		// Throws std::logic_error if another thread is executing a system in the
		// source or if its callables are already being copied. Reentrant copies
		// from the executing thread are allowed.
		Scheduler(const Scheduler &other);
		// Strong exception guarantee: a failed copy leaves this scheduler intact.
		// The same concurrent-copy restrictions as the copy constructor apply.
		Scheduler &operator=(const Scheduler &other);
		// Transfers the owned registration state and its timing names.
		Scheduler(Scheduler &&other) noexcept;
		// Transfers registrations and their owned timing-name storage.
		// Concurrent operations on either scheduler must be externally synchronized.
		Scheduler &operator=(Scheduler &&other) noexcept;

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
		// Like RunPhases, may throw std::logic_error on conflicting callback
		// execution. A nested Tick clears timings for the retained state. A
		// failure does not roll back the clock, timings or completed systems.
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
		// A run retains its schedule across registration changes and assignment.
		// A later system observes a replacement made before its invocation.
		// Throws std::logic_error if a callable is already running on another
		// thread, or if callback admission overlaps a copy of this scheduler.
		// Nested execution of a callable on its current thread is allowed.
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
		// next non-const operation on this Scheduler. Callers must synchronize
		// access against concurrent execution and timing changes.
		//
		// @return Timing rows for systems that have run since the last clear.
		const std::vector<Timing> &Timings() const;

		// Returns the number of registered systems.
		//
		// @return The current registration count across all phases.
		size_t SystemCount() const;

	  private:
		enum class Execution : uint8_t;
		struct Registered;
		struct Wave;
		struct TimingGroup;
		struct TimingRegistration;
		struct State;
		struct Invocation;
		static constexpr size_t NO_TIMING_ROW = static_cast<size_t>(-1);
		using Schedule = std::array<std::vector<Wave>, static_cast<size_t>(Phase::Count)>;

		static thread_local Invocation *ActiveInvocation;
		std::shared_ptr<State> GetState() const;
		static SystemScheduleIssue BuildSchedule(State *state, Schedule &schedule);
		static void EnsureSchedule(State *state);
		static TimingRegistration PrepareTimingRegistration(State *state, std::string_view name);
		static void RebindTimingNames(State *state);
		static void RecordTiming(State *state, size_t systemIndex, float milliseconds);
		static void RecordTimingLocked(State *state, size_t systemIndex, float milliseconds);
		static void ClearStateTimings(State *state);
		static void
		RunStatePhases(const std::shared_ptr<State> &state, Store &store, Phase first, Phase last);
		mutable std::shared_ptr<State> State_;
	};
} // namespace engine::ecs
