#pragma once

// A score for `Authority::SetPriority`, built from two things this module is
// still not allowed to know.
//
// `Authority`'s hook has existed since v0.4 and nothing has ever filled it in,
// so the rotation has been in sole charge and the order is a plain round robin.
// The reason it stayed empty is written in `SetPriority`'s own documentation:
// **the two scores worth having are read off a transform, and `replication`
// carries named components without knowing which of them is a position.**
//
// That argument is right and this does not overturn it. What it does is
// separate the *arithmetic* — which is generic — from the *lookup*, which is
// not. A caller supplies two accessors, one for where a client is looking and
// one for where an entity is; this supplies the falloff, the guards and the
// ordering behaviour. `replication` still names no component and links no
// simulation module.
//
// **What this is not.** It is not interest: `SetInterest` decides what may be
// sent at all and a score only decides what goes first. Nor is it a line of
// sight — that needs a broad phase and a game's own idea of an occluder, and it
// belongs wherever the host keeps one.
//
// @tier L12 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/replication/Authority.hpp>

#include <functional>

namespace engine::replication {

	// Scores entities by how near they are to a client's viewpoint.
	//
	// Built by a host and handed to `Authority::SetPriority`. Copyable, because
	// `std::function` takes a copy.
	//
	// @since v0.9
	struct DistancePriority {
		// Where a client is looking from, in world space.
		//
		// Answers `false` for a client with no viewpoint — one that has just
		// joined, or a spectator the host does not place. **Every entity then
		// scores the same**, which is the round robin this replaces and the
		// right answer rather than pretending the client is at the origin.
		std::function<bool(ClientId, core::Vector3 &)> Viewpoint;

		// Where an entity is, in world space.
		//
		// Answers `false` for an entity with no position. Scores zero, which is
		// the bottom of the range — a thing with no place in the world has no
		// claim on a budget ahead of one that has.
		std::function<bool(ecs::Entity, core::Vector3 &)> Position;

		// How far away something has to be before it scores nothing, in metres.
		//
		// **The curve is `1 - distance / falloff`, clamped, and deliberately
		// linear.** An inverse square is what light does and is the wrong shape
		// here: it collapses to almost nothing within a few metres, so
		// everything past that range sorts identically and the rotation decides
		// anyway — which is the behaviour this exists to improve on. A straight
		// ramp keeps the whole range meaningful.
		//
		// Sixty-four metres because that is `scene::WorldBounds`' default half
		// extent, so the default covers the default world.
		float FalloffMetres = 64.0f;

		// Scores one entity for one client.
		//
		// **Never returns a non-finite value**, whatever the accessors do. A NaN
		// in a comparator is not a weak ordering and `std::sort` on one is
		// undefined; `Authority` guards this too, and a scorer that could not be
		// trusted on its own would be one every caller had to remember to wrap.
		//
		// @param client The client being served.
		// @param entity The entity being scored.
		// @return Zero to one, one being at the viewpoint.
		float operator()(ClientId client, ecs::Entity entity) const;
	};
}
