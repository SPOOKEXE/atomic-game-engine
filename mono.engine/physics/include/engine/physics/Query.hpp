#pragma once

// Asking a world what is where, against the real collider shapes.
//
// **These are the second `Raycast`, and the difference is the whole point.**
// `spatial::Raycast` answers against the proxy boxes in an index; this one
// answers against the sphere, the cylinder or the rotated box inside them, and
// calls `spatial`'s to find the candidates it then tests exactly. Both header
// comments say which they are, because reaching for the wrong one does not fail
// to compile and does not obviously fail at runtime — it returns an answer that
// is a box away from the right one.
//
// **The storage is the caller's**, exactly as it is in `spatial::Query.hpp`,
// and `spatial::QueryResult` is reused rather than reinvented. A system that
// queries every tick allocates nothing, and a second convention for saying "how
// many, and was there more" would be a second thing to get right.
//
// **Every query here is a read and safe from several threads at once.** That is
// not incidental: the design note expects a system that raycasts per entity,
// which means several workers holding one world's index. Nothing here writes to
// the world, and the scratch a grid walk needs is on the stack rather than on
// the resource for exactly that reason.
//
// @tier L8 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/spatial/Query.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// The smallest run of rows worth handing to another worker, for a system
	// whose body is a query.
	//
	// **Thirty-two, and the default of 4096 would be wrong by two orders of
	// magnitude.** `parallel::Jobs::DEFAULT_GRAIN` is a guess about a body that
	// does almost nothing; a raycast walks a grid, resolves candidates and runs
	// an exact test per candidate, so one row here is thousands of times the
	// work one row of `IntegrateMotion` is. `Jobs::MINIMUM_GRAINS` is 8, so
	// this also sets the count below which the whole span runs inline: 32 times
	// 8 is 256 rays, which is well under where a raycasting system repays a
	// dispatch.
	//
	// Chosen rather than measured, and the distinction is worth keeping: the
	// number a benchmark would refine is which side of a hundred it sits, and
	// `parallel/AGENTS.md` already says a query body wants a grain in the tens.
	inline constexpr size_t RAYCAST_GRAIN = 32;

	// How many broad-phase candidates one query may consider per index.
	//
	// The grid walk writes into a stack buffer this size, which is what keeps
	// these queries allocation-free and safe to run from several threads. A
	// query that finds more says so through `QueryResult::Overflowed` rather
	// than quietly answering from a prefix — a truncated overlap read as "and
	// nothing more" is a contact that never happens.
	inline constexpr size_t QUERY_CANDIDATE_LIMIT = 256;

	// One collider a query found.
	//
	// An `ecs::Entity` rather than the `uint64_t` a `core::RayHit` carries.
	// `spatial` cannot name an entity — it is L6 and the ECS is L3 above it —
	// so its hits carry whatever number the caller put into the index, which
	// for these two grids is an array subscript and not an entity at all.
	// Handing that number back as an id would be plausible and wrong.
	//
	// @since v0.4
	struct ColliderHit {
		// Which entity was hit.
		ecs::Entity Owner;

		// How far along the ray the entry is, in metres. Zero for a ray that
		// started inside the shape, matching `spatial`'s convention.
		float Distance = 0.0f;

		// Where the ray entered, in world space.
		core::Vector3 Position;

		// The outward surface normal at the entry, pointing away from the
		// collider that was hit.
		core::Vector3 Normal;
	};

	// Finds the nearest collider a ray meets.
	//
	// Candidates come from both indexes and are then tested against the exact
	// shape: a rotated box through its own inverse transform, a sphere and a
	// cylinder analytically. **That inverse is the thing this function exists
	// for** — every axis-aligned test in the engine passes whether or not it is
	// right, which is why `v02v03v04.md` §3.7 names a raycast against a rotated
	// box as one of the behavioural tests.
	//
	// @param store       The world to ask. Must hold a `PhysicsWorld`.
	// @param ray         Origin and **unit** direction. A ray with no direction
	//                    finds nothing.
	// @param maxDistance How far to travel, in metres. Zero or less finds
	//                    nothing.
	// @param mask        Which layers to consider. A collider is a candidate
	//                    when it shares any layer with this.
	// @param ignore      One collider to look straight through, or a null
	//                    entity. **The caster itself, which is the only thing
	//                    this is for and is why it is one entity rather than a
	//                    list.** A character asking what is under its feet has
	//                    to start the ray inside its own capsule — a ray that
	//                    begins exactly on a face is a coin flip about whether
	//                    it hits it, and the coin lands differently on two
	//                    machines — so the nearest hit is always itself and the
	//                    floor is never reached. Comparing the *result* against
	//                    the caster cannot fix that: the answer has already
	//                    been thrown away.
	//
	//                    A general ignore list is deliberately not offered.
	//                    Every caller that has wanted one has wanted exactly
	//                    this, and a span would put an allocation and a loop on
	//                    the inner test for a case nobody has.
	// @return The nearest hit, or nothing. There is no "invalid hit".
	// @threadsafe
	std::optional<ColliderHit> Raycast(
		const ecs::Store &store,
		const core::Ray &ray,
		float maxDistance,
		spatial::LayerMask mask = spatial::LayerMask::All(),
		ecs::Entity ignore = ecs::Entity{}
	);

	// Finds every collider whose exact shape overlaps an axis-aligned box.
	//
	// @param store The world to ask.
	// @param box   The volume to test, in world space.
	// @param mask  Which layers to consider.
	// @param found Where to write the entities, owned by the caller.
	// @return How many were written, and whether there were more.
	// @threadsafe
	spatial::QueryResult OverlapBox(
		const ecs::Store &store, const core::AABB &box, spatial::LayerMask mask, std::span<ecs::Entity> found
	);

	// Finds every collider whose exact shape overlaps a sphere.
	//
	// Against the shape and not its bound, which is the difference from
	// `spatial::OverlapSphere`: a cylinder standing in the corner of its own
	// box is reported by that one from a metre away, correctly, and by this one
	// only when the sphere really reaches it.
	//
	// @param store  The world to ask.
	// @param centre The middle of the sphere, in world space.
	// @param radius Its radius in metres. A negative radius finds nothing.
	// @param mask   Which layers to consider.
	// @param found  Where to write the entities, owned by the caller.
	// @return How many were written, and whether there were more.
	// @threadsafe
	spatial::QueryResult OverlapSphere(
		const ecs::Store &store,
		const core::Vector3 &centre,
		float radius,
		spatial::LayerMask mask,
		std::span<ecs::Entity> found
	);

	// Finds every collider a moving shape could meet on its way.
	//
	// **Conservative, and it says so rather than pretending to a time of
	// impact.** The volume tested is the box swept by the moving shape's own
	// world bound, and every candidate's exact shape is intersected with it —
	// so nothing on the path is ever missed, and a shape whose bound is much
	// larger than itself, a thin cylinder lying diagonally for instance, can
	// report a collider it would have passed beside.
	//
	// A first-time-of-impact sweep needs a distance function between two convex
	// shapes, which this module does not have and which `AGENTS.md` records as
	// deliberately absent rather than approximated. A caller needing the moment
	// of contact steps the shape and uses this to bound the search.
	//
	// @param store    The world to ask.
	// @param collider What is moving, read exactly as `scene::Collider` is.
	// @param from     Where it starts, in world space.
	// @param motion   How far and which way it travels, in metres. Zero motion
	//                 is an overlap and is answered as one.
	// @param mask     Which layers to consider.
	// @param found    Where to write the entities, owned by the caller.
	// @return How many were written, and whether there were more.
	// @threadsafe
	spatial::QueryResult ShapeCast(
		const ecs::Store &store,
		const scene::Collider &collider,
		const core::CFrame &from,
		const core::Vector3 &motion,
		spatial::LayerMask mask,
		std::span<ecs::Entity> found
	);
}
