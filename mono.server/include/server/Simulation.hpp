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

	// Populates `store` and registers the systems that move it. Deterministic:
	// the same count produces the same world, which is what lets two runs be
	// compared.
	void BuildPlaceholderWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t count);
}
