#pragma once

// The placeholder world the server hosts until there is a game file to load one
// from.
//
// **The components are `mono.engine/scene`'s and nothing here declares one.**
// This file used to carry a `Position`, a `Velocity` and a `WorldBounds`,
// because the ECS is storage and does not know what a Transform is and `scene`
// at L7 did not exist. It does now, both programs register the same set under
// the same names, and that is what lets a snapshot cross to a client with no
// translation layer - `mono.client/include/client/Replicated.hpp` used to be
// that layer and is not any more.
//
// Still deliberately not shared with `mono.client`. A client-tier header is
// invisible to this binary by construction, and reaching for one would be the
// first crack in the split this program exists to demonstrate. Sharing happens
// through an engine module, which is what `scene` is.
//
// What is left here is the placeholder: `Chatter` and `Heard`, which exist to
// put traffic on a bus that would otherwise have none, and the three functions
// that build and register the world.

#include <engine/core/Name.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>

namespace server {

	// Makes a placeholder world talk on a bus.
	//
	// **There is no game yet, so there is no traffic.** A universe split across
	// processes cannot be shown to work without something crossing it, and
	// inventing traffic inside a test would only prove the test. This is that
	// something: opt-in, off unless asked for, and it goes when a game file
	// arrives with real traffic of its own.
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

	// Registers the `scene` component set and this program's two placeholders,
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
	void RegisterPlaceholderComponents();

	// Registers the systems, without touching storage.
	//
	// Separate from BuildPlaceholderWorld because a restore needs exactly this
	// half: a snapshot carries state, never code, so a world restored from one
	// has entities and no behaviour until this runs. The same program registers
	// the same systems, which is what makes a replay reproduce a run rather
	// than approximate it.
	void RegisterPlaceholderSystems(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);

	// Populates `store` and registers the systems that move it. Deterministic:
	// the same count produces the same world, which is what lets two runs be
	// compared.
	void BuildPlaceholderWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t count);

	// Gives a hosted world physics and weight - the server's half of what
	// `studio::Editor::PrepareWorld` does, less the presentation this binary
	// has no renderer for.
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
	// @since v0.13
	void PrepareSimulation(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);
}
