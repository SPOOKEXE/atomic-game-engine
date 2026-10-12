#pragma once

// Server simulation setup and optional cross-world bus diagnostics.

#include <engine/core/Name.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>

namespace server {

	// Makes a world talk on a bus when --chatter is enabled.
	//
	// A universe split across processes needs observable traffic to check its
	// bus. This diagnostic is opt-in and independent of scene authorship.
	//
	// @since v0.2
	struct Chatter {
		// Published on and subscribed to. One topic, so every world with this
		// resource hears every other.
		engine::core::Name Topic;
	};

	// What a chattering world has heard.
	//
	// A resource rather than a log line, so a test - or a person with a
	// debugger - can ask a world what reached it without parsing output.
	//
	// @since v0.2
	struct Heard {
		// Messages this world has received on the topic.
		uint64_t Count = 0;

		// The world the last one came from.
		engine::core::Name From;
	};

	// Registers the `scene` component set and this program's two bus resources,
	// under explicit names.
	//
	// Called before anything else, on every path - including a replay, which
	// loads a snapshot naming these types before any world has been built. A
	// process that had not registered them would resolve every name to nothing
	// and refuse the snapshot.
	//
	// Explicit names rather than the compiler's spelling, because these cross: a
	// recording written by one build is read by another, and whatever a compiler
	// happens to call a type in an anonymous namespace is not a promise anybody
	// made. The `scene` names are the same strings the client registers, which
	// is what lets a snapshot cross between the two programs unaltered.
	void RegisterServerComponents();

	// Installs the optional bus pump and entity-count metric.
	//
	// A replay restores storage but not scheduler entries, so its host calls
	// this again after the snapshot is loaded.
	void RegisterServerDiagnostics(engine::ecs::Scheduler &scheduler);

	// Gives a hosted world physics and weight - the server's half of what
	// `studio::Editor::PrepareWorld` does, less the presentation this binary
	// has no renderer for. A restored world keeps its physics resources and
	// clock; this call restores observation and scheduler setup around them.
	//
	// **The server was not simulating anything, and that is a stranger
	// sentence than it sounds.** `D00039` wired physics into the studio and
	// deliberately stopped there, so a game hosted headlessly integrated no
	// body and resolved no contact: a part dropped in a hosted world hung in
	// the air, and every client watching it agreed, because they were watching
	// an authority that was not simulating. It is also the precondition for
	// anything to *own* a simulation - an owner of nothing is a field.
	//
	// Both halves, for the reason the studio takes both: `physics` has no
	// gravity of its own by design, so the pipeline alone integrates every
	// body at zero acceleration for ever. `scene::Gravity` is the world's
	// answer and this is the host applying it.
	//
	// Call it once per world. `Scheduler::Add` takes a name and does not
	// deduplicate, so a world prepared twice integrates twice per tick - which
	// is a world running at double gravity rather than an error anybody sees.
	//
	// @param store            The world to prepare.
	// @param scheduler        Its scheduler.
	// @param physicsTickRate  Physics steps per second from the world's
	//                         settings, or zero to follow its tick rate.
	// @since v0.13
	void
	PrepareSimulation(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, double physicsTickRate);
}
