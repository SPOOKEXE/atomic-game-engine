#pragma once

// What a spatial query is asked, and what it answers with.
//
// A ray here is an origin and a **unit** direction, and how far to travel is a
// separate argument to whatever is being asked rather than a member. Nothing
// normalises on the caller's behalf: a query handed a direction of length two
// reports every distance at half its true value, and that error grows with how
// wrong the caller was instead of failing where the mistake was made.
//
// **`RayHit` has no suite of its own, deliberately.** It carries no behaviour —
// it is the shape a query fills in — so it is covered by the suites of the
// queries that produce it. That is the same exception the root `AGENTS.md`
// already names for `Entity.hpp`, and it is written down here so the gap reads
// as a decision rather than an oversight. `Ray` does have one, because
// `PointAt` is arithmetic and arithmetic can be wrong.
//
// Absent on purpose: no `FromPoints`, no maximum distance on the ray, and no
// cached reciprocal direction. The first two are one line of the caller's own
// arithmetic. The third belongs to the slab test that consumes it — one ray is
// tested against many boxes, so the reciprocal is a loop invariant of the query
// and not a property of the ray.
//
// @tier L1 · shared

#include <engine/core/types/Vector3.hpp>

#include <cstdint>

namespace engine::core {

	// A half-line: where a query starts and which way it goes.
	//
	// `Direction` must have length one. A default-constructed ray has no
	// direction at all and hits nothing, which mirrors `Vector3::Unit`
	// returning `Zero` for a zero vector — saying "there is no direction" beats
	// a NaN that surfaces three subsystems away.
	//
	// @since v0.4
	struct Ray {
		// Where the ray begins, in the caller's distance unit.
		Vector3 Origin;

		// Which way it points. Length one; nothing here checks or corrects it.
		Vector3 Direction;

		// Constructs the degenerate ray at the origin with no direction.
		constexpr Ray() = default;

		// Constructs a ray from an origin and a direction that is already unit length.
		//
		// @param origin    Where the ray begins.
		// @param direction A unit-length direction. Pass `Vector3::Unit()` of
		//                  whatever you have rather than the raw difference of
		//                  two points.
		constexpr Ray(const Vector3 &origin, const Vector3 &direction)
			: Origin(origin), Direction(direction) {}

		// Returns the point `distance` units along the ray.
		//
		// Exact only for a unit direction, which is the contract on the type.
		//
		// @param distance How far along, in the same unit as `Origin`.
		constexpr Vector3 PointAt(float distance) const {
			return Origin + Direction * distance;
		}

		// Reports whether both the origin and the direction are exactly equal.
		constexpr bool operator==(const Ray &other) const {
			return Origin == other.Origin && Direction == other.Direction;
		}
	};

	// One thing a ray met.
	//
	// There is no validity flag, because a query that found nothing returns no
	// hit rather than a hit that says it is not one. A flag makes the caller's
	// mistake — reading the position out of a miss — compile and produce a
	// plausible number.
	//
	// @since v0.4
	struct RayHit {
		// What was hit, as the caller's own identifier.
		//
		// A `uint64_t` and not an `ecs::Entity`: this type is L1 and the ECS is
		// L3, so the id a query carries is whatever the caller put into the
		// index. A caller holding entities converts on the way in and on the
		// way out.
		uint64_t Id = 0;

		// How far along the ray the hit is, in the ray's distance unit.
		float Distance = 0.0f;

		// Where the hit is, in world space.
		Vector3 Position;

		// The outward surface normal at the hit, pointing away from what was hit.
		Vector3 Normal;
	};
}
