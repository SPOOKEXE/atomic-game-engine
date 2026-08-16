#pragma once

// @tier L12 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/replication/Authority.hpp>

#include <functional>

namespace engine::replication {

	// Scores entities by how near they are to a client's viewpoint.
	//
	// @since v0.9
	struct DistancePriority {
		// Where a client is looking from, in world space.
		//
		std::function<bool(ClientId, core::Vector3 &)> Viewpoint;

		// Where an entity is, in world space.
		//
		std::function<bool(ecs::Entity, core::Vector3 &)> Position;

		// How far away something has to be before it scores nothing, in metres.
		//
		float FalloffMetres = 64.0f;

		// Whether the line between a client and an entity is blocked.
		//
		// **Read by `Refine` and never by `operator()`.** Answering it is a
		// raycast against the host's broad phase, which is three orders of
		// magnitude dearer than the subtraction the distance costs - so the two
		// are two calls, registered through `Authority::SetPriority` and
		// `Authority::SetPriorityRefinement`, and only the cheap one is asked
		// about every entity. Asking this one about every entity for every
		// client measured at 51% of a two-hundred-client tick.
		std::function<bool(ClientId, ecs::Entity)> Blocked;

		// What a hidden entity keeps of its score, zero to one.
		//
		float HiddenFraction = 0.25f;

		// The score below which occlusion is not even asked about.
		//
		float OcclusionFloor = 0.1f;

		// Scores one entity for one client by distance alone.
		//
		// @param client The client being served.
		// @param entity The entity being scored.
		// @return Zero to one, one being at the viewpoint.
		float operator()(ClientId client, ecs::Entity entity) const;

		// Lowers a score for an entity the client cannot see.
		//
		// **Only ever lowers**, which is what lets the caller ask it about the
		// rows in contention and treat an unrefined score as an upper bound
		// everywhere else - see `Authority::SetPriorityRefinement`.
		//
		// @param client The client being served.
		// @param entity The entity being scored.
		// @param score  What `operator()` gave.
		// @return The score, multiplied by `HiddenFraction` when blocked.
		//
		// @since v0.16
		float Refine(ClientId client, ecs::Entity entity, float score) const;
	};
}
