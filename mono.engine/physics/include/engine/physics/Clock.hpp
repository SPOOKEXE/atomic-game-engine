#pragma once

// How often a world steps its physics, independently of how often it ticks.
//
// A world's tick rate is what its scripts, its characters and its change
// signals run at. Physics is the most expensive thing inside that tick and the
// least sensitive to running at a different rate, so the two are separated: a
// world of a thousand crates can solve at 30 while everything else stays at 60,
// and a world of six precise moving platforms can solve at 120.
//
// **A second accumulator rather than `core::FixedTimestep`, and the difference
// is what it is fed.** That one turns wall seconds into ticks. This one turns
// *simulated* seconds into steps, so the number of steps a recorded run
// produces is a property of the recording rather than of the machine replaying
// it. Everything else about it - the fixed delta, the remainder carried to the
// next tick, the cap that gives up on a stall instead of chasing it - is the
// same argument, and the constants below say so.
//
//     BeginPhysicsTick(store);            // once per world tick
//     while (BeginPhysicsStep(store)) {   // zero, one or several
//         Integrate(store);               // PhysicsStepSeconds(store) is the delta
//     }
//
// @tier L8 · shared

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// A world's physics rate, the simulated time owed to it, and what that has
	// cost so far.
	//
	// Held as a `Store` resource, so it is per world by construction and a
	// second world in the same process cannot reach it.
	//
	// @since v0.17
	struct PhysicsClock {
		// A tick owing more steps than this is given up on rather than caught
		// up, for `core::FixedTimestep::MAXIMUM_TICKS_PER_FRAME`'s reason: a
		// tick that takes longer than its own budget arrives at the next one
		// owing more, and simulation time then falls permanently behind and
		// never recovers. Giving up is visible; the spiral is not.
		static constexpr int32_t MAXIMUM_STEPS_PER_TICK = 8;

		// The highest rate that can be set.
		//
		// **A ceiling so that everything derived from the rate is finite by
		// construction.** A rate arrives from a game file and from a snapshot,
		// both of which `docs/CODE_QUALITY.md` §7 calls hostile, and an
		// infinite one makes the owed step count infinite too - which is
		// undefined behaviour when it is cast to an `int32_t`, not a large
		// number. The value is far above anything `MAXIMUM_STEPS_PER_TICK`
		// would let a world actually run, so it constrains nothing real.
		static constexpr double MAXIMUM_RATE = 100'000.0;

		// Steps per second. Zero follows the world's tick rate, which is what a
		// world that never says otherwise wants - and what every world in this
		// repository is.
		//
		// The one number that survives a save file. Everything below is derived
		// from it and from the ticks that have run since.
		double Rate = 0.0;

		// Simulated seconds accumulated and not yet spent on a step.
		double Accumulator = 0.0;

		// The length of the step currently running.
		//
		// **Every step reads this rather than `Store::Time().Delta`**, which is
		// the world's tick and no longer the physics step's. `PhysicsStepSeconds`
		// is the read, and it falls back to the tick when no step is running so
		// that calling a step directly - which every suite in this module does -
		// behaves as it always has.
		float Delta = 0.0f;

		// Steps still owed inside the tick currently running.
		int32_t Owed = 0;

		// Which step of the current tick is running, counting from one.
		//
		// **Read by `NarrowPhase` to decide whether to clear the contact
		// events.** The manifolds belong to a step and are cleared on every
		// one; the events belong to the *tick*, because a reader asks "what
		// touched this tick" and a touch that began on a world's second
		// physics step of a tick is not a touch that did not happen.
		int32_t StepInTick = 0;

		// Whether a step is running right now. Read by `physics.contacts` to
		// find out whether `physics.simulation` stepped at all this tick.
		bool Stepping = false;

		// Steps run since the world was prepared.
		uint64_t Steps = 0;

		// Steps abandoned to `MAXIMUM_STEPS_PER_TICK`.
		//
		// Worth surfacing for `FixedTimestep::Dropped`'s reason: it is the
		// difference between "the machine is slow" and "physics is silently
		// running slower than its stated rate", and those need different
		// responses.
		uint64_t DroppedSteps = 0;
	};

	// Sets how often this world steps its physics.
	//
	// Called by whatever built the world, from the world's authored settings.
	// The rate is creation-time in practice - `world::WorldSettings` has no
	// runtime setter - but changing it here is safe at any point between ticks:
	// the accumulator is kept, so the step after a change is the first one of
	// the new length rather than a jump.
	//
	// @param store          The prepared world.
	// @param stepsPerSecond Steps per second, or zero to follow the tick rate.
	//                       Anything that is not above zero - a negative, a
	//                       NaN - reads as zero, and anything above
	//                       `MAXIMUM_RATE` is held there.
	void SetPhysicsTickRate(ecs::Store &store, double stepsPerSecond);

	// How often this world steps its physics.
	//
	// @param store The world to read.
	// @return The configured rate, zero when it follows the tick rate, and zero
	//         for a world that was never prepared.
	double PhysicsTickRate(const ecs::Store &store);

	// The delta a physics step should integrate over.
	//
	// @param store The world to read.
	// @return The running step's length, or `Store::Time().Delta` when no step
	//         is running or the world has no clock.
	float PhysicsStepSeconds(const ecs::Store &store);

	// The clock, for a caller that wants the counters rather than the delta.
	//
	// @param store The world to read.
	// @return The clock, or `nullptr` for a world that was never prepared.
	const PhysicsClock *PhysicsClockOf(const ecs::Store &store);

	// Charges one world tick to the clock and works out how many steps it owes.
	//
	// Called once per tick by `physics.simulation`, before the first
	// `BeginPhysicsStep`. Uses `Store::Time().Delta`, which is the tick the
	// world just advanced.
	//
	// @param store The prepared world.
	void BeginPhysicsTick(ecs::Store &store);

	// Claims one of the steps this tick owes.
	//
	// @param store The prepared world.
	// @return `true` when a step should run, having set the delta
	//         `PhysicsStepSeconds` reports. `false` once the tick is spent, and
	//         `false` for a world that has no clock.
	bool BeginPhysicsStep(ecs::Store &store);

	// Whether the step running now is the first of its tick.
	//
	// What belongs to a tick rather than to a step is cleared here and nowhere
	// else. A world with no clock answers `true`, because every step it runs is
	// the first and only one of its tick.
	//
	// @param store The world to read.
	// @return `true` on the first step of a tick, and for a world with no clock.
	bool FirstPhysicsStepOfTick(const ecs::Store &store);
}
