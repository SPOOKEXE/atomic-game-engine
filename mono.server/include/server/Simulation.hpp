#pragma once

// The placeholder world the server hosts until there is a game file to load one
// from.
//
// Components and resources live here for the same reason the client's do: the
// ECS is storage and does not know what a Transform is, and `scene` at L7 —
// which will own the real Basic Components set — does not exist yet. They move
// there at v0.3.
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

	struct Position {
		engine::core::Vector3 Value;
	};

	struct Velocity {
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
		float HalfExtent = 64.0f;
	};

	// Populates `store` and registers the systems that move it. Deterministic:
	// the same count produces the same world, which is what lets two runs be
	// compared.
	void BuildPlaceholderWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t count);
}
