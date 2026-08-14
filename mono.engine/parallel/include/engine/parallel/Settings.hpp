#pragma once

// The settings this module owns, and the one call that applies them.
//
// **Declared and applied here rather than in `core::Config`**, because `core`
// is below this and a settings layer that reached upwards to apply everybody's
// values would be an inverted dependency wearing a helpful hat. `Config` owns
// the sources and the precedence; each module owns its own names.
//
// Two settings, both of which were reachable only by a command-line flag on one
// program:
//
// - `engine.jobs.workers` — how many worker threads the process-wide pool
//   starts, or zero to choose from the hardware. A deployment sharing a machine
//   between several servers is the case that wants it, and there was no way to
//   say so.
// - `engine.serial-compute` — every parallel dispatch runs inline. `--force
//   -serial-compute` on the client was the only way to reach it, and the studio
//   and the server had no equivalent at all.
//
// @tier L2 · shared
// @since v0.15

namespace engine::parallel {

	// Declares `engine.jobs.workers` and `engine.serial-compute`.
	//
	// @return `false` when a name collided, which is a bug in a table.
	bool DeclareFlags();

	// Applies the serial-compute switch.
	//
	// Called after `core::Config::Apply` and before anything dispatches.
	void ApplyFlags();

	// What `engine.jobs.workers` says, or zero for "work it out".
	//
	// **Read where `Jobs::Start` is called rather than applied here**, because
	// the automatic answer is `WorkersPerHost(processes)` and only the program
	// knows how many processes are sharing the machine — the server works it
	// out from its own placement plan. So the shape at every call site is
	//
	//     const unsigned configured = parallel::ConfiguredWorkers();
	//     Jobs::Start(configured != 0 ? configured : WorkersPerHost(hosts));
	//
	// and `WorkersPerHost` stays the pure function its suite pins exact numbers
	// against.
	//
	// @return The configured count, or zero.
	unsigned ConfiguredWorkers();
}
