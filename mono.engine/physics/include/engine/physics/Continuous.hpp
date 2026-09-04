#pragma once

// arch-waiver public-header: forward physics API. Simulation hosts retain this
// complete continuous-collision contract.

// Stopping a body at the wall it would otherwise have gone through.
//
// **The gap `AGENTS.md` recorded as deliberately absent until v0.17.** A body
// moving further than its own thickness in one step is on the far side of a thin
// wall by the time the narrow phase looks, and there is nothing there to find -
// the contact never existed at any moment the pipeline sampled. `Solver.hpp`'s
// `MAXIMUM_CORRECTION_SPEED` bounds the *correction* rather than the motion, so
// it does not help: the body was never overlapping.
//
// What closes it is a distance function between two convex shapes, which is what
// `src/ConvexQuery.hpp` now is. This step is the consumer.
//
// Both bodies may translate and rotate. A retained grid indexes conservative
// swept bounds, then conservative advancement finds the first shared time of
// impact for each candidate pair. Static geometry uses the same walk with zero
// velocity on its side.
//
// @tier L8 · shared

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// How far a body may travel in one step, as a fraction of its own thinnest
	// half-extent, before it is swept.
	//
	// **Its thinnest half-extent, because that is what decides what it can pass
	// through.** A long thin plank travelling along its own length is not going
	// anywhere it could not have been seen going; the same plank travelling
	// edge-on is the thing that ends up inside a wall. So the test is against the
	// dimension that is smallest, not against the motion's own axis - which
	// would need a support query per body per tick to answer and would only be a
	// more precise version of a threshold that is already a judgement.
	//
	// One, so a body is swept the moment it moves further than half its own
	// thinnest dimension. That is comfortably before it could reach the far side
	// of anything of its own size, and it is far enough above the ordinary
	// falling speeds - a body under gravity moves about 2 mm on the tick it is
	// dropped and about 0.16 m a tick at terminal velocity - that a scene of
	// settling crates sweeps nothing at all.
	inline constexpr float CONTINUOUS_MOTION_RATIO = 1.0f;

	// How far *into* the surface a swept body is left, in metres.
	//
	// **Into, and the first version of this was `in front of`, which hangs the
	// body in mid-air.** Stopping a bullet a fraction of a millimetre short of a
	// wall leaves two shapes that do not overlap, so the narrow phase finds
	// nothing, so the solver never touches the velocity - and next tick the body
	// starts from where it was clamped, still doing three hundred metres a
	// second, and is clamped to the same place again. The bullet hovers against
	// the wall forever and nothing in the pipeline is wrong.
	//
	// So the clamp puts the body just past the moment of contact instead. The
	// ordinary narrow phase reports it, the solver applies a normal impulse the
	// way it does for every other contact - which is what makes a bullet bounce
	// or stop according to its material rather than according to this step - and
	// the position correction unwinds the overlap.
	//
	// A millimetre: twice `PENETRATION_SLOP`, so the contact is certainly
	// reported and certainly corrected, and far below anything visible.
	//
	// **This step still never writes a velocity.** Where the body is, is this
	// step's decision. What it does next is the solver's, and a continuous step
	// that stopped bodies dead would be a second physics with no material
	// behind it.
	inline constexpr float CONTINUOUS_BITE = 0.001f;

	// Clamps every body that moved far enough to have passed through something.
	//
	// `Phase::Simulation`, between `IntegrateMotion` and `SyncBroadphase`: after
	// the positions have been stepped, so there is a motion to sweep, and before
	// the ordinary broad-phase index is rebuilt.
	//
	// It reads the static index that the previous sync built. Dynamic swept bounds
	// are rebuilt here because both endpoints and the path between them move.
	//
	// **It writes `scene::Transform` through the reference `Each` hands out**,
	// never through `Store::Set`, for the reason `AGENTS.md` gives at length: a
	// write through `Set` stamps the row, and `SyncBroadphase` reads those stamps
	// to decide whether *static* geometry moved.
	//
	// **The velocity is left alone.** This step decides where the body is, not
	// how fast it is going; the contact it has been placed *into* is what the
	// narrow phase reads on the same tick, and taking the velocity away here
	// would make a fast body stop dead against a wall it should have bounced
	// off. See `CONTINUOUS_BITE`.
	//
	// @param store The world to sweep.
	// @tick
	void SweepFastBodies(ecs::Store &store);
}
