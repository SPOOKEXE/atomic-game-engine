#pragma once

// A convex polyhedron, and the one question a narrow phase asks it.
//
// **The support function is the whole interface and the faces are the extra.**
// Everything a general convex-convex test needs is "how far does this shape
// reach along a direction, and at which point" - `SupportPoint` below - and that
// question is answerable from an unordered point set with no hull at all. The
// faces exist for the two things a point set cannot do: give a contact *manifold*
// wider than a point, and be drawn.
//
// **So a caller that only tests overlap does not need `Build` to have run well.**
// A hull whose faces came out coarse still answers every support query exactly,
// because the support of a point set and the support of its hull are the same
// number. That is worth knowing before reading the builder: its failure mode is
// a worse manifold, never a missed contact.
//
// @tier L5 · shared
// @since v0.17

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::collision {

	// One planar face of a hull, as a run of vertex indices.
	//
	// Wound counter-clockwise seen from outside, which is what makes `Normal`
	// derivable from the winding and is what a triangle fan for drawing needs.
	//
	// @since v0.17
	struct HullFace {
		// Where this face's indices start, as an index into `ConvexHull::Loops`.
		uint32_t FirstIndex = 0;

		// How many there are. Three or more.
		uint32_t IndexCount = 0;

		// The outward unit normal.
		//
		// **Stored rather than derived per query**, because a face's plane is
		// asked for once per separating-axis candidate and deriving it is a
		// cross product and a normalise over indices that are not adjacent in
		// memory.
		core::Vector3 Normal;

		// The plane's distance from the origin along `Normal`, so a point `p` is
		// outside when `Normal.Dot(p) > Offset`.
		float Offset = 0.0f;
	};

	// A convex polyhedron in its own object space.
	//
	// @since v0.17
	struct ConvexHull {
		// The hull's corners, and nothing inside it.
		//
		// **`Build` discards interior points, and that is most of what it is
		// for.** A support query is a scan, so the cost of every narrow-phase
		// question is the length of this array - a thousand-vertex mesh reduced
		// to the twenty corners of its hull is fifty times cheaper per query,
		// for the same answer.
		std::vector<core::Vector3> Points;

		// The faces, in no particular order.
		std::vector<HullFace> Faces;

		// The vertex indices every face's loop is cut from.
		std::vector<uint32_t> Loops;

		// The object-space bound.
		//
		// **Derived and never stored on disk**, which is `assets::MeshData`'s
		// rule and holds here for its reason: a stored bound is a second copy of
		// a fact the points already carry, and the copy is the one a crafted
		// file gets to choose. A shape claiming a bound of zero disappears from
		// every broad phase.
		core::AABB Bounds;

		// Whether this hull has any volume at all.
		//
		// **A flat or degenerate result is a legitimate outcome and not an
		// error.** A caller can hand this a mesh whose vertices are collinear, a
		// single triangle, or two points, and it will get back a hull that
		// supports every query correctly and has no faces. What it must not do
		// is assume `Faces` is non-empty; see `Build`.
		bool Solid() const {
			return !Faces.empty();
		}
	};

	// The furthest point of the hull along a direction.
	//
	// **A linear scan, deliberately.** A hill-climb over an adjacency list is
	// asymptotically better and is slower at every size this engine has: a
	// baked hull is tens of points, which is one or two cache lines, and the
	// climb costs an indirection per step to save arithmetic that is already
	// running four-wide.
	//
	// `direction` need not be unit length; only its sign structure is read.
	//
	// **A hull with no points returns the origin**, which is the only answer
	// that is not a guess, and is what makes an unbuilt shape harmless rather
	// than undefined.
	//
	// @param hull      The shape.
	// @param direction Which way to reach.
	// @return The extreme point, in the hull's own space.
	core::Vector3 SupportPoint(const ConvexHull &hull, const core::Vector3 &direction);

	// How far the hull reaches along a direction from its own origin.
	//
	// The support function's scalar half, for a caller doing a separating-axis
	// test rather than building a simplex.
	//
	// @param hull      The shape.
	// @param direction A unit direction.
	// @return The largest projection of any point onto `direction`.
	float SupportDistance(const ConvexHull &hull, const core::Vector3 &direction);

	// How far apart two points have to be to count as different corners.
	//
	// **A length and not a ratio**, because the input is world-scale geometry in
	// metres and a ratio would make the builder's behaviour depend on how far
	// from its own origin a model was authored. A tenth of a millimetre is below
	// what any mesh this engine imports resolves and well above the float noise
	// in a cross product of metre-scale vectors.
	inline constexpr float HULL_WELD_DISTANCE = 0.0001f;

	// How many points a built hull may keep.
	//
	// **A bound on an allocation and on a scan, for the reason
	// `spatial::HashGrid::MAXIMUM_CELLS_PER_PROXY` has one.** A support query is
	// linear in the point count and runs several times per contact per
	// iteration, so a hull of ten thousand corners is not a detailed collider,
	// it is a frame that does not finish. Past this the builder stops adding
	// points and returns what it has - which is still a convex shape containing
	// every point it did accept, and still smaller than the mesh.
	//
	// **Sixty-four because that is where the useful shapes are.** A crate is
	// eight, a barrel is thirty-two, a rock is under sixty; a character mesh
	// wants a decomposition into several hulls rather than one hull with three
	// hundred corners, and that is a different feature.
	inline constexpr size_t MAXIMUM_HULL_POINTS = 64;

	// Builds the convex hull of a point cloud.
	//
	// **Quickhull, and the degenerate cases are the specification rather than
	// an afterthought.** Real input is a baked mesh, and baked meshes are flat
	// planes, single quads, and models whose vertices were welded to a grid -
	// so "the points are coplanar" and "two of the four seed points coincide"
	// are the ordinary cases, not the hostile ones. Every one of them produces
	// a hull that answers `SupportPoint` exactly:
	//
	// - **Fewer than four distinct points**, or all of them within
	//   `HULL_WELD_DISTANCE` of one plane, one line or one point: the points are
	//   kept, `Faces` is empty, and `Solid()` is false. Support queries and the
	//   bound are still exact.
	// - **More than `MAXIMUM_HULL_POINTS` corners**: the build stops early. The
	//   result is convex and contains every point it accepted; it does not
	//   contain the ones it did not, so a caller wanting a *bounding* hull has
	//   to check `Solid()` and the point count itself.
	// - **Any coordinate that is not finite**: dropped before the build, because
	//   one infinity makes every plane test meaningless and the result would be
	//   a hull that swallows the world.
	//
	// **Deterministic.** The seed points are chosen by extent with the input
	// index breaking a tie, the remaining points are visited in input order, and
	// nothing here reads a clock, an address or a hash. Two builds of one point
	// cloud produce the same hull, which is what lets a baked hull be compared
	// between machines.
	//
	// @param points    The cloud, in any order.
	// @param tolerance How far outside a face a point must be to count as
	//                  outside it. Zero or below takes `HULL_WELD_DISTANCE`.
	// @return The hull.
	ConvexHull BuildConvexHull(std::span<const core::Vector3> points, float tolerance = 0.0f);
}
