#pragma once

// Server-authoritative shot encoding and sphere hit testing.
// @tier L10 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::examples {

	// Client shot input.
	struct Shot {
		// Where it was aimed from and along, in world space.
		core::Ray Aim;

		// How far it reaches, in metres.
		float Range = 100.0f;
	};

	// Untrusted client-selected range ceiling.
	constexpr float MAXIMUM_SHOT_RANGE = 1000.0f;

	// Encodes a shot.
	std::vector<std::byte> EncodeShot(const Shot &shot);

	// Decodes a shot, rejecting malformed or unsafe client input.
	//
	// @param bytes The payload.
	// @param out   Filled on success, left alone otherwise.
	// @return `false` on anything malformed.
	bool DecodeShot(std::span<const std::byte> bytes, Shot &out);

	// One shot target at the rewound position.
	struct Target {
		ecs::Entity Entity;

		// Position at the shooter's simulation time.
		core::Vector3 At;

		// Bounding-sphere radius.
		float Radius = 0.5f;
	};

	// Shot result.
	struct Hit {
		// The entity, or an invalid one when nothing was hit.
		ecs::Entity Entity;

		// How far along the ray, in metres.
		float Distance = 0.0f;

		// Whether anything was hit at all.
		bool Struck = false;
	};

	// Finds the nearest hit, with deterministic entity-id tie breaking.
	Hit NearestHit(const Shot &shot, std::span<const Target> targets);
}
