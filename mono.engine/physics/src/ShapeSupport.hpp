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
}
