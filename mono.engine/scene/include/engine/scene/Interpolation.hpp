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
}
