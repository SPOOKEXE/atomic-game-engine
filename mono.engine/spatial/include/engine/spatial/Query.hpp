#pragma once

// Asking a hash grid what is where.
//
// **Everything here answers against boxes.** A grid holds a `Proxy`, a proxy
// holds an `AABB`, and that is the whole of what these functions know about a
// shape. So a hit is a *candidate* rather than a contact, and a normal is the
// face of an axis-aligned box rather than the surface of anything. A capsule
// standing in the corner of its own bounding box is reported by
// `OverlapSphere` from a metre away, correctly, and it is the caller's job to
// decide that the exact shapes do not touch.
//
// **There will be two functions called `Raycast`.** This one hits proxy boxes
// in an index. The one `physics` will grow hits colliders, and will call this
// one to find the candidates it then tests exactly. Reaching for the wrong one
// does not fail to compile and does not obviously fail at runtime — it returns
// a plausible answer that is a box away from the right one.
//
// **The storage is the caller's.** Every query but one writes into a span the
// caller owns and returns what it learned about the writing, so a system that
// queries every tick allocates nothing. `CODE_FORMAT.md` bans an output
// parameter "where a return value works", and here it does not work: the
// container has to be the caller's, and `QueryResult` is the struct return that
// carries the rest. `Raycast` is the exception because there is at most one
// answer and it fits in the return — the same shape as `assets::Grant::Open`.
//
// @tier L6 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::spatial {

	// What a query did with the span it was given.
	//
	// **`Overflowed` is reported rather than inferred.** A span filled exactly
	// to its length and a span that ran out look identical from `Written`
	// alone, and a truncated overlap read as "and nothing more" is a contact
	// that never happens.
	//
	// @since v0.4
	struct QueryResult {
		// How many entries were written, from the front of the span.
		size_t Written = 0;

		// Whether there was more to report than the span could hold.
		bool Overflowed = false;
	};

	// Finds the nearest proxy box a ray meets.
	//
	// The nearest, not the first found: the walk visits cells in a fixed order
	// that has nothing to do with distance, so every candidate is tested and
	// the closest kept. Two candidates at the same distance resolve in walk
	// order, which is stable for a given grid and input.
	//
	// @param grid        The index to ask.
	// @param ray         Origin and **unit** direction. A ray with no direction
	//                    finds nothing.
	// @param maxDistance How far to travel, in metres. Zero or less finds
	//                    nothing.
	// @param mask        Which layers to consider. A proxy is a candidate when
	//                    it shares any layer with this.
	// @return The nearest hit, or nothing. There is no "invalid hit".
	// @threadsafe
	std::optional<core::RayHit>
	Raycast(const HashGrid &grid, const core::Ray &ray, float maxDistance, LayerMask mask = LayerMask::All());

	// Finds every proxy box a ray meets, nearest first.
	//
	// When the span cannot hold them all it keeps the **nearest**, which is the
	// half a caller almost always wants, and says that it dropped the rest.
	//
	// @param grid        The index to ask.
	// @param ray         Origin and unit direction.
	// @param maxDistance How far to travel, in metres.
	// @param mask        Which layers to consider.
	// @param hits        Where to write, owned by the caller. Written from the
	//                    front, in increasing distance.
	// @threadsafe
	QueryResult RaycastAll(
		const HashGrid &grid,
		const core::Ray &ray,
		float maxDistance,
		LayerMask mask,
		std::span<core::RayHit> hits
	);

	// Finds every proxy whose box overlaps `box`, touching included.
	//
	// @param grid  The index to ask.
	// @param box   The volume to test, in world space.
	// @param mask  Which layers to consider.
	// @param found Where to write the ids, owned by the caller.
	// @threadsafe
	QueryResult
	OverlapBox(const HashGrid &grid, const core::AABB &box, LayerMask mask, std::span<uint64_t> found);

	// Finds every proxy whose box comes within `radius` of `centre`.
	//
	// The test is against the box, so this is the distance to the nearest point
	// of a proxy's bounds and not to whatever shape is inside them.
	//
	// @param grid   The index to ask.
	// @param centre The middle of the sphere, in world space.
	// @param radius Its radius in metres. A negative radius finds nothing.
	// @param mask   Which layers to consider.
	// @param found  Where to write the ids, owned by the caller.
	// @threadsafe
	QueryResult OverlapSphere(
		const HashGrid &grid,
		const core::Vector3 &centre,
		float radius,
		LayerMask mask,
		std::span<uint64_t> found
	);

	// Sweeps an axis-aligned box along `motion` and finds what it would meet.
	//
	// **An axis-aligned box and only that.** A shape-typed sweep needs
	// `scene::ShapeKind`, which is L7, and this module is L6 — so the sweep
	// that knows about spheres and cylinders belongs to `physics`, layered on
	// top of this one. Nothing here holds a rotation either, so there is no
	// oriented sweep to be had at this layer even in principle.
	//
	// Zero motion is `OverlapBox`, and is answered by calling it.
	//
	// @param grid   The index to ask.
	// @param box    Where the swept box starts, in world space.
	// @param motion How far and which way it travels, in metres.
	// @param mask   Which layers to consider.
	// @param found  Where to write the ids, owned by the caller.
	// @threadsafe
	QueryResult ShapeCast(
		const HashGrid &grid,
		const core::AABB &box,
		const core::Vector3 &motion,
		LayerMask mask,
		std::span<uint64_t> found
	);
}
