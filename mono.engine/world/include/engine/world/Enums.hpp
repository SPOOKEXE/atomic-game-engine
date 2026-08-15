#pragma once

// The named states a universe talks in.
//
// Every one of these crosses a boundary - a supervisor reads a world's state, a
// deployment picks an isolation, a host is configured with an execution mode -
// so each is a type rather than a bool or an int that loses its meaning at the
// first hop.
//
// **Names are the format, numbers are not.** Anything written to a file, a wire
// or a log carries the name; the underlying values are free to move.
//
// @tier L4 · shared

#include <cstdint>

namespace engine::world {

	// Whether a world is ticking, and why not when it is not.
	//
	// @since v0.2
	enum class WorldState : uint8_t {
		// Ticking at its full rate.
		Active,

		// Ticking at a reduced rate. A subarea with nobody in it still has to
		// advance - crops grow, timers expire - but not sixty times a second.
		Idle,

		// Not ticking at all, and costing nothing but its storage. The whole
		// point of suspending is that a universe of a thousand subareas pays
		// for the handful somebody is standing in.
		Suspended,

		// Stopped because its tick threw. Restored from its snapshot by the
		// supervisor, or held down if it keeps faulting.
		Faulted,

		// Held by a supervised host, not by this process.
		//
		// The driver keeps a record of it so that the buses, the directory and
		// the recording still know the world exists - a topic it subscribed to
		// and a teleport addressed to it are the driver's to route. What the
		// driver does *not* have is its storage, so it never ticks it and never
		// reads its store.
		//
		// Distinct from `Suspended`, which is a world this process holds and
		// has chosen not to tick. Blurring the two would make "not ticking
		// here" mean two different things at the same time.
		Remote,
	};

	// What a world needs from the process it runs in.
	//
	// Soft faults are quarantined per world either way. This is only about
	// surviving a *neighbour's* hard fault, which no amount of care inside a
	// process can arrange.
	//
	// @since v0.2
	enum class Isolation : uint8_t {
		// Shares a host with other worlds. The default, because a process per
		// subarea does not scale to hundreds of them.
		Shared,

		// Gets a host to itself. For the persistent overworld, and for anything
		// running content nobody has audited.
		Dedicated,
	};

	// How a host spends its worker pool.
	//
	// The switch changes no result - it decides where parallelism is taken, not
	// what is computed. A test asserts that, because a tuning knob that quietly
	// changed behaviour would be a semantic pretending to be a setting.
	//
	// @since v0.2
	enum class ExecutionMode : uint8_t {
		// One job batch over the worlds. Right for a host holding many worlds
		// that are each below the intra-world parallel crossover, which is most
		// of them.
		WorldParallel,

		// Worlds in a plain loop, each free to use the whole pool for its own
		// parallel systems. Right for a host holding one large world.
		WorldSerial,
	};

	// Why a world could not be created, or why an operation was refused.
	//
	// @since v0.2
	enum class WorldStatus : uint8_t {
		// It worked.
		Ok,

		// No world is registered under that name or id.
		NoSuchWorld,

		// A world already exists under that name. Names identify one thing.
		NameTaken,

		// The name was empty. A world with no name cannot be addressed, and
		// everything that crosses a boundary addresses by name.
		NoName,

		// The operation needs the driver thread and was called from a tick.
		WrongThread,
	};

	// Whether a world in this state is advanced by `Universe::Tick`.
	//
	// **Two states tick and three do not, and the count is the reason this is a
	// function.** Callers wrote `state == WorldState::Active` because that is
	// the state they had in mind, and an `Idle` world - which ticks, slowly -
	// then answered "no". A caller deriving how far between two ticks to draw
	// got the wrong answer for a world that was simulating perfectly well.
	//
	// **It says nothing about whether the universe is being ticked at all.** A
	// host that has stopped calling `Universe::Tick` leaves every world in
	// whatever state it was in, and no state can express that; the caller knows
	// and this cannot. `studio::PresentationAlpha` is the caller that needs
	// both halves and takes them separately.
	//
	// @param state The state to ask about.
	// @return Whether the driver advances a world in this state.
	// @since v0.11
	bool Ticks(WorldState state);

	// Returns a stable, human-readable name for a world state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(WorldState state);

	// Returns a stable, human-readable name for an isolation level.
	//
	// @param isolation The level to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Isolation isolation);

	// Returns a stable, human-readable name for an execution mode.
	//
	// @param mode The mode to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ExecutionMode mode);

	// Returns a stable, human-readable name for a world status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(WorldStatus status);
}
