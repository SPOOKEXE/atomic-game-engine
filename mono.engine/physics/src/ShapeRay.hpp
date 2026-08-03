#pragma once

// A ray against one exact collider shape. Private to this module.
//
// The three conventions are `spatial/src/RayBox.hpp`'s, deliberately and to the
// letter, because a caller mixing the two tests must not have to hold two sets
// of rules:
//
// - **The distance is the entry distance.** A ray starting inside reports zero.
// - **The normal is the surface entered through**, pointing out of the shape.
// - **Only an entry within `maxDistance` counts.**
//
// Not in `core`, for the reason `RayBox.hpp` gives about itself: the
// conventions are properties of the query rather than of a shape, and these
// tests have exactly one caller.

#include "ShapeSupport.hpp"

#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>

namespace engine::physics {

	// What a ray did to one shape.
	struct ShapeHit {
		// Whether it met the shape at all, within the distance asked for.
		bool Touched = false;

		// How far along the ray the entry is, clamped at zero for an origin
		// inside.
		float Distance = 0.0f;

		// The outward normal of the surface entered through, in world space.
		core::Vector3 Normal;
	};

	// Intersects a ray with a placed collider, keeping the entry.
	//
	// @param shape       The collider and where it is.
	// @param ray         Origin and unit direction.
	// @param maxDistance How far the caller is travelling, in metres.
	// @return The entry, or a miss.
	ShapeHit IntersectRayShape(const ShapeInstance &shape, const core::Ray &ray, float maxDistance);
}
