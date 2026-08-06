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
		std::function<bool(ClientId, ecs::Entity)> Blocked;

		// What a hidden entity keeps of its score, zero to one.
		//
		float HiddenFraction = 0.25f;

		// The score below which occlusion is not even asked about.
		//
		float OcclusionFloor = 0.1f;

		// Scores one entity for one client.
		// @param client The client being served.
		// @param entity The entity being scored.
		// @return Zero to one, one being at the viewpoint.
		float operator()(ClientId client, ecs::Entity entity) const;
	};
}
