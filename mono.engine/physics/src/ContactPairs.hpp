#pragma once

// The six exact pair functions, and the one place a normal is ever flipped.
//
// All six are analytic: box-box, box-sphere, box-cylinder, sphere-sphere,
// sphere-cylinder, cylinder-cylinder, written here. There is no
// physics library behind this and there is not going to be one.
//
// # The convention, stated once
//
// **Every pair function takes its two shapes in `scene::ShapeKind` order and
// reports a normal pointing from the first toward the second.** Contact points
// sit on the *second* shape's surface, and penetration is never negative.
//
// `ContactBetween` is the only function that reorders and the only one that
// flips a normal. That matters more than it looks: the pipeline names its two
// bodies by entity id, the pair functions name theirs by shape, and the two
// orders disagree for half of all pairs. Flipping in six places instead of one
// is how two of the six end up disagreeing about which way is out - which
// reads as objects occasionally flying apart rather than as a sign error.
//
// # What the axis sets are, and where they stop being exact
//
// A box is a polytope, so its fifteen face and edge-cross axes are provably the
// whole set: box-box is exact. A sphere is analytic against everything. A
// cylinder is neither - it is smooth, so its minimum-penetration direction can
// point anywhere, and no finite list of axes is complete for it.
//
// The list here covers every contact the design names: cap on face, barrel on
// face, cap on cap, barrel on barrel both crossed and parallel, box edge on
// barrel, box corner on barrel, and box corner on rim. What it does not
// enumerate is a box *edge* meeting a cap's rim obliquely. `ProjectionRadius`
// is exact for whatever direction it is handed, so a missing axis never invents
// depth out of nothing - the failure mode is the opposite one, reporting a
// shallow contact between two shapes that are in fact a fraction of a
// millimetre apart in that one configuration. Read `AGENTS.md` in this
// directory before widening or narrowing the set.

#include "FaceManifold.hpp"
#include "ShapeSupport.hpp"

#include <engine/core/types/Vector3.hpp>

#include <cstddef>

namespace engine::physics {

	// One direction the separating-axis search will try.
	struct AxisCandidate {
		// Which way to look. Need not be unit; a zero-length one is skipped,
		// which is how a degenerate cross product removes itself.
		core::Vector3 Direction;

		// Whether this is a face or cap normal of one of the two shapes.
		//
		// A primary axis wins ties, and keeps them by a margin. A cross-product
		// axis that beats a face by a hair is almost always float noise on a
		// resting contact, and taking it swaps the manifold from four points to
		// one for a tick - which is the jitter the whole multi-point design is
		// there to remove.
		bool Primary = false;
	};

	// What the separating-axis search decided.
	struct AxisChoice {
		// Unit, pointing from the first shape toward the second.
		core::Vector3 Normal;

		// The overlap along `Normal`, in metres, and never negative.
		float Depth = 0.0f;

		// Whether every axis tried found overlap. **False means separated**,
		// which is a result rather than a failure to decide.
		bool Touching = false;
	};

	// Tries every candidate axis and keeps the one they overlap least along.
	//
	// Stops at the first axis that separates them, because one is proof.
	//
	// @param first  The shape the normal points away from.
	// @param second The shape it points toward.
	// @param axes   The directions to try.
	// @param count  How many of them there are.
	// @return The chosen axis, or a choice marked not touching.
	AxisChoice LeastOverlap(
		const ShapeInstance &first, const ShapeInstance &second, const AxisCandidate *axes, size_t count
	);

	// Where two placed colliders touch, in the pipeline's A/B convention.
	//
	// **The only function that reorders and the only one that flips.** `first`
	// is the collider on the smaller entity id, so the normal it returns points
	// from that body toward the other and its points lie on the other's
	// surface - which is what `ContactManifold` promises.
	//
	// @param first  The collider on the smaller entity id.
	// @param second The collider on the larger.
	// @return The contact, or a solution marked not touching.
	// Whether a shape kind carries baked geometry rather than an extent.
	//
	// The one place the two new kinds are named together, so a third one is a
	// change here rather than a search for every `||`.
	//
	// @since v0.17
	constexpr bool Baked(scene::ShapeKind kind) {
		return kind == scene::ShapeKind::Hull || kind == scene::ShapeKind::Mesh ||
			   kind == scene::ShapeKind::Capsule;
	}

	// The contact between two shapes at least one of which is baked.
	//
	// **The general route, and it is one function rather than seven more arms.**
	// Adding `Hull` and `Mesh` to the exact table would have cost a pair function
	// against every existing kind and against each other - and every one of them
	// would have had to be exact for a shape that is not centrally symmetric,
	// which is the property the whole axis search rests on. GJK finds the axis
	// and EPA the depth; `ManifoldBetween` then builds the points out of the same
	// face clip every other pair uses, so a hull resting on a box gets the
	// four-point manifold that keeps it still.
	//
	// **A mesh is solved triangle by triangle**, because a triangle soup is not
	// convex and a soup's support point means nothing. Each triangle that the
	// moving shape's bound reaches is a three-point hull, and the deepest
	// contacts across them become the manifold.
	//
	// @param first  One shape, placed.
	// @param second The other.
	// @return The contact, normal pointing from `first` toward `second`.
	// @since v0.17
	ContactSolution ConvexContact(const ShapeInstance &first, const ShapeInstance &second);

	// Finds the nearest surfaces of a separated pair, including a convex shape
	// against a triangle soup. The distance cap bounds the mesh triangle walk.
	SeparatedContact
	SeparatedBetween(const ShapeInstance &first, const ShapeInstance &second, float maximumDistance);

	ContactSolution ContactBetween(const ShapeInstance &first, const ShapeInstance &second);

	// The six, in `scene::ShapeKind` order. Each obeys the convention above.
	//
	// Public to this module so a suite can put a known answer against one pair
	// without building a store, which is the only way to tell a wrong normal in
	// one pair from a wrong flip in the dispatcher.
	//
	// @param first  The first shape, per the function's name.
	// @param second The second shape.
	// @return The contact, or a solution marked not touching.
	ContactSolution BoxBox(const ShapeInstance &first, const ShapeInstance &second);

	// @copydoc BoxBox
	ContactSolution BoxSphere(const ShapeInstance &first, const ShapeInstance &second);

	// @copydoc BoxBox
	ContactSolution BoxCylinder(const ShapeInstance &first, const ShapeInstance &second);

	// @copydoc BoxBox
	ContactSolution SphereSphere(const ShapeInstance &first, const ShapeInstance &second);

	// @copydoc BoxBox
	ContactSolution SphereCylinder(const ShapeInstance &first, const ShapeInstance &second);

	// @copydoc BoxBox
	ContactSolution CylinderCylinder(const ShapeInstance &first, const ShapeInstance &second);
}
