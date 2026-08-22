#pragma once

// What a collider looks like from one direction.
//
// Two questions, and all six pair functions are built out of them: how far the
// shape reaches along an axis, and which piece of its surface faces that way.
//
// Private, and deliberately so. `AGENTS.md` in this directory lists support
// mappings under "not here yet, so do not add half of one" precisely because
// surface with no caller is a maintenance cost with nothing on the other side -
// they arrive here, in `src/`, with the pair functions that need them.
//
// **All three shapes are centrally symmetric, and the narrow phase is built on
// that.** A box, a sphere and a cylinder are each their own reflection through
// their centre, so the shadow one casts on any axis is an interval centred on
// the shadow of its centre. The separating-axis test collapses to
// `radiusA + radiusB - |offset . axis|` with no support point evaluated at all
// - exact for *every* direction rather than only the ones a polytope's faces
// name, and one expression for all six pairs. It is why the cylinder cases are
// additions to the box-box machinery rather than a second approach.

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Enums.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

	// **`ShapeInstance` moved to the public `Shapes.hpp` at v0.17**, and the
	// support functions below stayed here. The type is what a collider *is*
	// once it is somewhere, which is that header's subject; what is private is
	// the machinery that asks it questions. The move happened because
	// `PhysicsWorld` has to hold an array of them - see `SyncBroadphase`, which
	// fills one - and a resource in a public header cannot name a private type.

	// How many points one support feature may hold.
	//
	// Eight, and the number comes from the cylinder: a cap is a circle and a
	// polygon clipper needs a polygon. An inscribed octagon is within four per
	// cent of the disc's radius, which is finer than the contact tolerance and
	// still cheap to clip against. A box face is four points and a barrel is
	// two.
	inline constexpr size_t MAXIMUM_FEATURE_POINTS = 8;

	// The piece of a shape's surface that faces one direction.
	struct SupportFeature {
		// The vertices, wound counter-clockwise about `Plane` whenever there
		// are three or more of them. The winding is what makes the clipper's
		// edge planes point inward, so reversing it silently clips everything
		// away.
		core::Vector3 Points[MAXIMUM_FEATURE_POINTS];

		// The outward normal of the plane the points lie in.
		//
		// Only meaningful with three or more points. A segment or a vertex
		// takes the query direction instead, so a caller never has to test
		// `Count` before reading it.
		core::Vector3 Plane;

		// How many of `Points` are live: 1 for a vertex, 2 for an edge, 3 or
		// more for a face.
		size_t Count = 0;

		// Which feature of the shape this is, as a small stable number.
		//
		// **A cache key, not geometry.** `ContactPoint::Feature` has to name
		// the same physical contact next tick so the solver can warm-start from
		// last tick's impulse, and the two shapes' feature ids are the half of
		// that key the geometry contributes. A box numbers its six faces, a
		// cylinder its two caps and its barrel, a sphere has one.
		uint8_t Id = 0;
	};

	// The world-space direction of a cylinder's barrel.
	//
	// Local +Y, per `Shapes.hpp`. Meaningless for the other two shapes.
	//
	// @param shape The cylinder.
	// @return The unit barrel axis.
	inline const core::Vector3 &BarrelAxis(const ShapeInstance &shape) {
		return shape.Axis[1];
	}

	// Half the width of a shape's shadow on `axis`.
	//
	// Exact for every direction, because all three shapes are centrally
	// symmetric - see the file comment. `axis` must be unit length; nothing
	// here normalises it, and a longer one scales the answer.
	//
	// **Out of line on purpose.** Inlining it into `LeastOverlap` was measured
	// and was slower: the shape kind is loop-invariant, but the compiler will
	// not unswitch the loop to prove it, so all thirty calls got their own copy
	// of the three-way branch and the axis search grew past what its registers
	// hold.
	//
	// @param shape The collider to measure.
	// @param axis  A unit direction.
	// @return Half the extent of the projection, in metres.
	float ProjectionRadius(const ShapeInstance &shape, const core::Vector3 &axis);

	// The furthest point of a shape along `direction`.
	//
	// @param shape     The collider to measure.
	// @param direction A unit direction.
	// @return The support point, in world space.
	core::Vector3 SupportPoint(const ShapeInstance &shape, const core::Vector3 &direction);

	// The surface feature facing `direction`.
	//
	// **A box always answers with a whole face** rather than with the vertex or
	// edge the direction strictly selects. The clipper and the depth filter
	// throw away the points that are not really in contact, and a direction a
	// hair off a face normal would otherwise collapse a resting contact to one
	// point - which is the jitter the multi-point manifold exists to remove.
	//
	// @param shape     The collider to ask.
	// @param direction A unit direction pointing out of the shape.
	// @return The feature, with at least one point.
	SupportFeature FaceTowards(const ShapeInstance &shape, const core::Vector3 &direction);

	// A world-space point expressed in a shape's own axes.
	//
	// `CFrame::Inverse` would build a whole frame to do this; the pair
	// functions call it per collider per axis test, so the conjugate is written
	// out. The rotation must be unit length, which is the contract on `CFrame`.
	//
	// @param frame The frame to enter.
	// @param point A world-space point.
	// @return The same point in `frame`'s local axes.
	core::Vector3 ToLocalPoint(const core::CFrame &frame, const core::Vector3 &point);

	// A world-space direction expressed in a shape's own axes.
	//
	// The rotation half of `ToLocalPoint`, for a vector that has no position.
	//
	// @param frame  The frame to enter.
	// @param vector A world-space direction.
	// @return The same direction in `frame`'s local axes.
	core::Vector3 ToLocalVector(const core::CFrame &frame, const core::Vector3 &vector);

	// How many triangles of one mesh a single query may examine.
	//
	// **A limit and not a budget**: a body resting on terrain touches four, a
	// body teleported inside a mountain touches thousands, and the second must
	// reach a limit rather than an allocator. Past it the triangles examined are
	// the first this many the overlap reported, which is ascending by triangle
	// index - so the answer is a function of the mesh rather than of the order a
	// walk happened to take.
	//
	// Shared by the contact path and the sweep, because it is the same argument
	// about the same mesh and two numbers would be two answers to "why did it
	// stop at sixty-four".
	inline constexpr size_t MAXIMUM_MESH_TRIANGLES = 64;

	// The world-space box a shape reaches, whatever kind it is.
	//
	// **A hull is measured by its own points and everything else by its
	// extent**, which is the one thing `ShapeWorldBounds` cannot do on its own:
	// a hull's `Extent` is not read - `Shapes.hpp` says so - so a hull asked
	// through the extent path would answer with whatever the author left there.
	//
	// @param shape The placed collider.
	// @return The axis-aligned box that contains it.
	// @since v0.19
	core::AABB ShapeReach(const ShapeInstance &shape);

	// One triangle of a mesh, as a hull the general search can take.
	//
	// **A hull of three points, filled into a buffer the caller owns.** A
	// triangle is convex, so its support function is the same scan a hull's is -
	// and building it per triangle rather than baking one hull per triangle is
	// what keeps a terrain mesh from being a hundred thousand `ConvexHull`
	// objects.
	//
	// **Filled rather than returned, and that is not a style choice.** A
	// `ShapeInstance` holds a *pointer* to its hull, so a function returning the
	// two together by value hands back an instance pointing at the temporary it
	// was built in - which reads correctly, compiles, and is a use-after-free on
	// every call. The caller owns the hull and the pointer never leaves its
	// frame.
	//
	// A three-point hull has no faces, so `FaceTowards` gives its support
	// *point* and the manifold against it is a single contact. That is correct
	// for a triangle: three points cannot hold a box flat on their own, and what
	// holds it flat is the several triangles under it each contributing one.
	//
	// @param mesh     The mesh to read from.
	// @param triangle Its index, as `OverlapTriangles` reports one.
	// @param hull     Filled with the triangle's three points.
	// @since v0.19
	void FillTriangleHull(const collision::TriangleMesh &mesh, size_t triangle, collision::ConvexHull &hull);
}
