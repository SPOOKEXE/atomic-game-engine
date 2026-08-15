#pragma once

// The ray against axis-aligned box test. Private to this module.
//
// Not a member of `core::AABB`, and that is deliberate: it has exactly one
// caller, and the three conventions below are properties of the *query* rather
// than of a box. Putting them on the value type would put them a long way from
// the comment that documents them.
//
// The conventions, which every caller depends on:
//
// - **The distance is the entry distance**, where the ray first meets the box.
//   A ray that starts inside reports zero rather than the distance to the far
//   face, because a query asking what it is touching wants "here", not "over
//   there".
// - **The normal is the face the ray entered through**, pointing out of the
//   box and therefore against the ray on that axis. For an origin already
//   inside, it is the face whose plane the ray last crossed on the way in;
//   there is no entered face, and this is the nearest thing to one.
// - **Only the entry within `maxDistance` counts.** A box further along the
//   line than the caller asked to travel is not a hit.
//
// The slab method itself is standard and the structure here follows the shape
// every implementation of it has. The part worth reading is the parallel case.

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>

#include <cmath>
#include <limits>

namespace engine::spatial {

	// Below this a direction component counts as parallel to its pair of planes.
	//
	// Far below anything a unit direction can hold on all three axes at once,
	// so a real ray never takes the parallel branch by mistake. Chosen well
	// clear of the point where the reciprocal itself becomes infinite - a
	// float smaller than about 1e-38 inverts to an infinity, and the whole
	// point of the branch is that no infinity is ever multiplied.
	inline constexpr float PARALLEL_EPSILON = 1e-20f;

	// One divided by a ray's direction, and which axes have no direction at all.
	//
	// Computed once per ray rather than once per box. A raycast tests one ray
	// against many candidates, so this is the query's loop invariant - which is
	// why it lives here and not on `core::Ray`, where it would be a cached
	// field that has to be kept true.
	struct RayReciprocal {
		// One divided by each direction component, or zero on a parallel axis
		// where the value is never read.
		float Inverse[3] = {0.0f, 0.0f, 0.0f};

		// Whether each axis is parallel to its pair of planes.
		bool Parallel[3] = {false, false, false};

		explicit RayReciprocal(const core::Vector3 &direction) {
			const float components[3] = {direction.X, direction.Y, direction.Z};
			for (int axis = 0; axis < 3; axis++) {
				Parallel[axis] = std::abs(components[axis]) < PARALLEL_EPSILON;
				Inverse[axis] = Parallel[axis] ? 0.0f : 1.0f / components[axis];
			}
		}
	};

	// What a ray did to one box.
	struct BoxHit {
		// Whether the ray met the box at all within the distance asked for.
		bool Touched = false;

		// How far along the ray the entry is, clamped at zero for an origin inside.
		float Distance = 0.0f;

		// The outward normal of the face entered through.
		core::Vector3 Normal;
	};

	// Intersects a ray with a box, keeping the entry point.
	//
	// @param ray        Origin and unit direction.
	// @param reciprocal `RayReciprocal` for the same ray, computed once outside
	//                   the loop over candidates.
	// @param box        What to test against.
	// @param maxDistance How far the caller is travelling.
	inline BoxHit IntersectRayBox(
		const core::Ray &ray, const RayReciprocal &reciprocal, const core::AABB &box, float maxDistance
	) {
		const float origin[3] = {ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
		const float direction[3] = {ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
		const float minimum[3] = {box.Minimum.X, box.Minimum.Y, box.Minimum.Z};
		const float maximum[3] = {box.Maximum.X, box.Maximum.Y, box.Maximum.Z};

		float entry = -std::numeric_limits<float>::infinity();
		float exit = std::numeric_limits<float>::infinity();
		int entryAxis = -1;

		for (int axis = 0; axis < 3; axis++) {
			if (reciprocal.Parallel[axis]) {
				// **The branch that has to be here.** Dividing by zero gives an
				// infinity, and the comparisons below cope with an infinity
				// perfectly well - right up until the ray's origin sits exactly
				// on one of the two planes. Then the numerator is zero, zero
				// times infinity is a NaN, and a NaN compares false in both
				// directions: the miss is not detected and neither is the hit.
				//
				// So the axis is answered without arithmetic. Either the origin
				// lies between the planes, in which case this axis constrains
				// nothing, or it does not and no part of the ray is ever inside
				// the box.
				if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
					return BoxHit{};
				}
				continue;
			}

			float near = (minimum[axis] - origin[axis]) * reciprocal.Inverse[axis];
			float far = (maximum[axis] - origin[axis]) * reciprocal.Inverse[axis];
			if (near > far) {
				// Travelling the negative way along this axis meets the maximum
				// plane first.
				const float swapped = near;
				near = far;
				far = swapped;
			}

			if (near > entry) {
				entry = near;
				entryAxis = axis;
			}
			if (far < exit) {
				exit = far;
			}
		}

		// Parallel on every axis, so the ray has no direction and crossed no
		// plane. `Ray` documents a direction-less ray as hitting nothing, and
		// this is where that holds for a ray that got past the caller's guard.
		if (entryAxis < 0) {
			return BoxHit{};
		}

		// The three ways to miss: the slabs never overlap, the whole box is
		// behind the origin, or it is further away than the caller is going.
		if (entry > exit || exit < 0.0f || entry > maxDistance) {
			return BoxHit{};
		}

		BoxHit hit;
		hit.Touched = true;
		hit.Distance = entry > 0.0f ? entry : 0.0f;

		// Out of the box and against the ray: travelling in +X enters through
		// the face whose outward normal is -X.
		const float sign = direction[entryAxis] > 0.0f ? -1.0f : 1.0f;
		hit.Normal = core::Vector3{
			entryAxis == 0 ? sign : 0.0f,
			entryAxis == 1 ? sign : 0.0f,
			entryAxis == 2 ? sign : 0.0f,
		};
		return hit;
	}
}
