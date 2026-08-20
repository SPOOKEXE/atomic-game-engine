#pragma once

// The one system every presenting host runs and nothing else does.
//
// **Here rather than copied into each program, which is where it was.**
// `examples::LoadScene` installed a `CapturePrevious` and the studio needed the
// same five lines; a second copy of a system that writes a component is worse
// than a second copy of a helper, because the two can be installed into one
// world and then both write `PreviousTransform` with the second winning
// silently every tick. `Components.hpp` owns both types, so the loop over them
// belongs beside them.
//
// @tier L7 · shared

#include <engine/ecs/Store.hpp>

namespace engine::scene {

	// Copies every `Transform` into its `PreviousTransform`.
	//
	// **Runs in `Phase::PreSimulation`, before anything moves.** Rendering
	// interpolates from here, so this has to be the position at the start of
	// *this* tick - capturing it afterwards would interpolate from a place
	// nothing was ever at, which reads as a scene that stutters at exactly one
	// frame of delay and is invisible in a screenshot.
	//
	// A plain function capturing nothing, so it is replayable from a recording
	// and reusable by a second world.
	//
	// @param store The world.
	void CapturePreviousTransforms(ecs::Store &store);

	// Collapses `PreviousTransform` onto `Transform` for anything that has been
	// through a hole since this host last drew it.
	//
	// **The other half of the crossing fix, and the half that crosses a
	// machine.** `CrossPortals` maps a crossing body's `PreviousTransform`
	// through the seam so the frames between one tick and the next blend inside
	// the destination room. That is right and it is local: `PreviousTransform`
	// has no wire form, so a client receives a `Transform` that has jumped a
	// hundred units while holding a previous one from the room the body left,
	// and blends straight across the gap. At three frames to a tick the
	// character is drawn once or twice somewhere in between, which reads as a
	// body streaking across the world every time anybody walks through a portal.
	//
	// **A snap rather than a mapped blend, because a replica cannot map.** The
	// seam that carried the body is the authority's knowledge; what arrives here
	// is a position, and the honest thing to draw for a position that teleported
	// is the position. One tick of lost smoothing at the moment of a crossing is
	// invisible next to the streak it replaces.
	//
	// **Nothing to send.** `scene::PortalTransit` already crosses the wire for
	// the camera's benefit, so this reads a serial that is already there rather
	// than adding a packet - which is what the "do we need a portal move packet"
	// question turned out to want.
	//
	// Runs in `Phase::PreRender` on whichever host is presenting, before the
	// draw list is built. Idempotent, and free on a world where nobody has
	// crossed anything: one integer compare per body that has ever been through
	// a hole, and nothing at all for every other row.
	//
	// @param store The world being drawn.
	// @return How many bodies were snapped.
	size_t SnapPortalTransit(ecs::Store &store);
}
