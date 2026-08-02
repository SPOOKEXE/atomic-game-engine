#pragma once

// The placeholder world the server hosts until there is a game file to load one
// from.
//
// Components and resources live here for the same reason the client's do: the
// ECS is storage and does not know what a Transform is, and `scene` at L7 —
// which will own the real Basic Components set — does not exist yet. They move
// there at v0.4.
//
// Deliberately not shared with `mono.client`. A client-tier header is invisible
// to this binary by construction, and reaching for one would be the first crack
// in the split this program exists to demonstrate. When these become shared,
// they become `scene`, not an include across two programs.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>

namespace server {

	// Where an entity is.
	//
	// Not the client's Transform, and not a copy of it that drifted. The two
	// programs each own their placeholder components until `scene` at L7 owns
	// both — see this module's AGENTS.md on why that is the design and not
	// duplication waiting to be factored out.
	struct Position {
		// Metres from the origin, in world space.
		engine::core::Vector3 Value;
	};

	// How fast an entity is going, and which way.
	struct Velocity {
		// Metres per second. Integrated once per tick against the fixed delta,
		// never against measured elapsed time — a tick is a function of its
		// state, so a recorded run replays.
		engine::core::Vector3 Value;
	};

	// The cube the entities bounce around inside, so that a long run stays
	// bounded instead of drifting to infinity and denormalising.
	//
	// A resource, not a component. It was a component holding the same four
	// bytes on every entity — which is a shared fact stored per-entity, the
	// exact shape GARG_ECS_Layout.md §5 says to hoist. Making it a resource
	// takes a column out of the archetype and a load out of the bounce loop's
	// inner body, and it makes "the world is 128 wide" a property of the world
	// rather than something 4096 entities happen to agree on.
	struct WorldBounds {
		// Half the cube's width, in metres, so the world spans twice this on
		// each axis. Stored as the half because that is the form the bounce
		// check wants, and deriving it per entity per tick is arithmetic nobody
		// needs to repeat.
		float HalfExtent = 64.0f;
	};

	// Registers this program's component types, under explicit names.
	//
	// Called before anything else, on every path — including a replay, which
	// loads a snapshot that names these types before any world has been built.
	// A process that had not registered them would resolve every name to
	// nothing and refuse the snapshot.
	//
	// Explicit names rather than the compiler's spelling, because these cross:
	// a recording written by one build is read by another, and
	// `{anonymous}::%Position` is not a promise anybody made.
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
	// A resource rather than a log line, so a test — or a person with a
	// debugger — can ask a world what reached it without parsing output.
	//
	// @since v0.2
	struct Heard {
		// Messages this world has received on the topic.
		uint64_t Count = 0;

		// The world the last one came from.
		engine::core::Name From;
	};

	// Registers this program's component types, under explicit names.
	//
	// Called before anything else, on every path — including a replay, which
	// loads a snapshot naming these types before any world has been built. A
	// process that had not registered them would resolve every name to nothing
	// and refuse the snapshot.
	//
	// Explicit names rather than the compiler's spelling, because these cross: a
	// recording written by one build is read by another, and whatever a compiler
	// happens to call a type in an anonymous namespace is not a promise anybody
	// made.
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
}
