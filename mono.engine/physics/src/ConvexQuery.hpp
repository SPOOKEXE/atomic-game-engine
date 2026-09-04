#pragma once

// How far apart two convex shapes are, and how deep they overlap.
//
// **The one thing `AGENTS.md` listed as absent that three features were waiting
// on.** A distance function between two convex shapes is what a hull collider
// needs to be tested against anything, what a triangle of a mesh collider needs,
// and what a time-of-impact sweep is built out of. Written once, here.
//
// **GJK for the distance and EPA for the depth**, which is the standard pairing
// and is chosen over widening the separating-axis search for a reason the
// existing narrow phase states from the other side: `ShapeSupport.hpp` says the
// SAT here collapses to `radiusA + radiusB - |offset . axis|` *because all three
// analytic shapes are centrally symmetric*. A convex hull is not. Feeding one to
// that expression is not a conservative answer, it is a wrong one, and the
// symptom is a rock that pushes back from the wrong side of itself.
//
// **This does not replace the six exact pairs.** Box against box is exact,
// cheap, and gives a four-point manifold out of its own face clip;
// `ContactBetween` routes to this only for the pairs it has no exact answer
// for. Refuse a change that sends box-box through here "for consistency" - it
// would be slower and less accurate at once.
//
// Everything is in world space, because the two shapes are in two frames and the
// answer has to be in one.

#include "ShapeSupport.hpp"

#include <engine/core/types/Vector3.hpp>

#include <cstddef>

namespace engine::physics {

	// How many support points GJK may evaluate before giving up.
	//
	// **A bound and not a tuning knob.** GJK converges in a handful of
	// iterations for every shape pair this engine has - the count is roughly the
	// number of features between the two closest ones - and a run that has not
	// terminated by here is one cycling between two simplices a float rounding
	// apart. Answering "not touching" at that point is the conservative half of
	// a wrong answer: a contact missed for one tick, rather than a loop that
	// does not end.
	inline constexpr size_t GJK_ITERATIONS = 32;

	// How many times EPA may expand its polytope.
	//
	// A box converges in well under a dozen rounds and a hull against a hull in
	// a few dozen. Past this the answer is the best plane found so far, which is
	// an *under*-estimate: the polytope is an inner approximation of the
	// Minkowski difference throughout, so stopping early reports a shallower
	// overlap than there is and never a deeper one. That is the direction the
	// failure has to run in - a shallow push is a body that sinks for a tick, a
	// deep one is a body thrown across the room.
	inline constexpr size_t EPA_ITERATIONS = 48;

	// How many faces EPA may hold.
	//
	// **A bound on an allocation, and it counts retired faces too.** Faces are
	// retired rather than erased, because the horizon walk refers to them by
	// index and compacting would invalidate every reference - so this is the
	// total ever created rather than the live count, and it has to be several
	// times `EPA_ITERATIONS` for the iteration budget to be reachable at all.
	//
	// **It was 128 and that was measurably too few.** The polytope reached it
	// after a dozen rounds and the early return handed back a *seed* face as the
	// answer: a normal at right angles to the true one, at a depth of nothing,
	// with no failure anywhere. `tests/ConvexQuery.cpp` checks the general
	// answer against the exact box-box pair over a sweep of placements, which is
	// what caught it.
	inline constexpr size_t EPA_FACES = 512;

	// How small a length counts as zero.
	//
	// Used for the degeneracy tests - a simplex with no area, a search direction
	// with no length - rather than as a convergence target. It is far below any
	// distance the rest of the pipeline can act on.
	inline constexpr float CONVEX_EPSILON = 1e-6f;

	// How much deeper a support point has to reach before EPA keeps expanding.
	//
	// **A tenth of a millimetre, and it is a length rather than the epsilon
	// above.** A polytope converges on a *flat* face exactly and on a curved one
	// asymptotically, so a sphere never satisfies a tolerance near float
	// precision - it just spends the whole iteration budget shaving nanometres
	// and then reports whatever the budget ran out on. This is fifth of
	// `PENETRATION_SLOP`, so the solver cannot act on a difference this small
	// anyway.
	inline constexpr float EPA_TOLERANCE = 1e-4f;

	// The closest points between two convex shapes.
	//
	// @since v0.17
	struct ConvexSeparation {
		// The closest point on the first shape, in world space.
		core::Vector3 OnFirst;

		// The closest point on the second shape, in world space.
		core::Vector3 OnSecond;

		// The gap between them, in metres. Never negative.
		float Distance = 0.0f;

		// **Whether they overlap, in which case the two points and the distance
		// mean nothing.** GJK answers "are they apart, and by how much"; a
		// caller that wants a depth asks `PenetrationBetween`, which is a
		// different and more expensive search.
		bool Overlapping = false;
	};

	// The gap between two convex shapes, or that there is none.
	//
	// **Gilbert-Johnson-Keerthi**, over the Minkowski difference
	// `support(d) = first(d) - second(-d)`. The origin lies in that set exactly
	// when the shapes overlap, and the closest point of the set to the origin is
	// the vector between the two closest surface points.
	//
	// @param first  One shape, placed.
	// @param second The other.
	// @return The separation, or `Overlapping`.
	ConvexSeparation ClosestPoints(const ShapeInstance &first, const ShapeInstance &second);

	// The shallowest translation that would separate two overlapping shapes.
	//
	// @since v0.17
	struct ConvexPenetration {
		// The unit direction, **pointing from the first shape toward the
		// second** - the convention every pair function in this module obeys,
		// and the one `ContactBetween` is the single place to flip.
		core::Vector3 Normal;

		// How far along `Normal` the second shape would have to move to stop
		// touching, in metres.
		float Depth = 0.0f;

		// Whether they overlap at all. **False is the answer and not the absence
		// of one**, matching `ContactSolution::Touching`.
		bool Overlapping = false;
	};

	// How deeply two convex shapes overlap, and which way to push.
	//
	// **The expanding polytope algorithm**, started from the tetrahedron GJK
	// leaves behind when it finds the origin inside the Minkowski difference.
	// The closest face of that polytope to the origin gives the direction and
	// the depth directly.
	//
	// **Shapes that only touch are reported as overlapping at zero depth**,
	// which is the honest answer and is what keeps a body resting exactly on a
	// surface from flickering between two answers.
	//
	// @param first  The shape the normal points away from.
	// @param second The shape it points toward.
	// @return The penetration, or `Overlapping` false.
	ConvexPenetration PenetrationBetween(const ShapeInstance &first, const ShapeInstance &second);

	// How many advances a sweep may make before giving up.
	//
	// Conservative advancement converges geometrically for shapes approaching
	// head on and slowly for a grazing pass, which is exactly the case where the
	// answer matters least. The translating sweep reports no hit at the cap. The
	// full-motion sweep below has its own conservative fallback.
	inline constexpr size_t SWEEP_ADVANCES = 32;

	// Full rotational sweeps use a looser angular speed bound than translation.
	// If this cap is spent, the last safe fraction is returned as a conservative
	// hit and marked on `ConvexSweep`.
	inline constexpr size_t MOTION_SWEEP_ADVANCES = 128;

	// How close counts as touching, when a sweep is looking for the moment of
	// contact.
	//
	// **A skin, and it is what makes the answer usable rather than exact.** A
	// sweep that advanced until the gap was zero would put the shape exactly on
	// the surface, where the narrow phase's own tolerance decides whether there
	// is a contact at all - so it would sometimes stop one tick short of the one
	// it was called to prevent. A quarter of a millimetre in front of the
	// surface is a contact the narrow phase certainly reports.
	inline constexpr float SWEEP_SKIN = 0.00025f;

	// When a moving convex shape first meets a fixed one.
	//
	// @since v0.17
	struct ConvexSweep {
		// How far along the motion, from zero to one.
		float Fraction = 1.0f;

		// Where they meet, on the fixed shape's surface.
		core::Vector3 Position;

		// The unit direction from the fixed shape toward the moving one at that
		// moment, which is the direction a contact would push the mover.
		core::Vector3 Normal;

		// Whether they meet at all along the motion.
		bool Hit = false;

		// The advance cap was spent, so `Fraction` is a safe stop rather than a
		// converged contact.
		bool ConservativeFallback = false;

		// Witness-point closing speed at the hit, in metres per second.
		float ClosingSpeed = 0.0f;
	};

	// The first moment a translating convex shape touches another.
	//
	// **Conservative advancement.** Ask how far apart the two shapes are, work
	// out the soonest that gap could possibly close given the motion, jump the
	// shape forward by exactly that much, and ask again. Every step is a lower
	// bound on the time of impact, so the walk never steps past a contact - and
	// it converges on one from below.
	//
	// **Translation only, and rotation is the stated limit.** A body that turns
	// while it travels sweeps a shape this does not model, so a long thin thing
	// spinning end over end can still pass through something. That is the honest
	// version of continuous collision for a solver whose integration is a linear
	// step plus an angular one; the alternative is a conservative advancement
	// that bounds the *angular* motion too, which needs a bound on how fast any
	// point of the shape can move and is a different function.
	//
	// **A shape that already overlaps reports a hit at zero**, which is the
	// answer that keeps a caller from stepping it further into whatever it is
	// already inside.
	//
	// @param moving The shape at the start of its motion.
	// @param motion How far and which way it travels, in metres.
	// @param fixed  What it might hit, which does not move.
	// @return When they first touch, or `Hit` false.
	ConvexSweep
	SweepConvex(const ShapeInstance &moving, const core::Vector3 &motion, const ShapeInstance &fixed);

	// The first moment two convex shapes meet while both translate and rotate.
	//
	// Angular reach is bounded by each shape's farthest point from its origin.
	// This makes every advance a lower bound on the time of impact even when the
	// nearest features change while the shapes turn.
	ConvexSweep SweepConvexMotion(
		const ShapeInstance &first,
		const core::Vector3 &firstLinear,
		const core::Vector3 &firstAngular,
		const ShapeInstance &second,
		const core::Vector3 &secondLinear,
		const core::Vector3 &secondAngular,
		float seconds
	);
}
