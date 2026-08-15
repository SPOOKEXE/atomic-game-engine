#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::render {

	// How many times over the authored surface size a pane needs, given how much
	// of the screen it covers.
	//
	// **A surface camera is fitted to its pane, so its texture maps one to one
	// onto the pane's screen footprint.** A pane covering half the screen
	// therefore wants half the screen's pixels, and handing it a fixed size
	// whatever it covers is what makes a portal go coarse as you walk up to it -
	// the texels are all there, they are just spread over a rectangle several
	// times larger than the one they were authored for.
	//
	// **Powers of two, because the alternative is reallocating a render target
	// every frame.** A continuous size would recreate two textures and a depth
	// buffer on every step the viewer takes; doubling gives at most a handful of
	// distinct sizes over the whole approach, and the step is where the
	// hysteresis below sits.
	//
	// **It only grows.** The authored size is the floor - a scene that asked for
	// a large mirror keeps it - and the screen is the ceiling, because nothing is
	// served by rendering more texels than the pane can occupy.
	//
	// **In a header of its own so a suite can reach it**, which is the whole
	// reason it is not still a lambda in the render path: what it is worth
	// testing about is a sequence of calls rather than any single answer, and
	// `Renderer::Render` needs a device to reach.
	//
	// @param authored The larger of the authored dimensions.
	// @param coverage 0..1 of the viewport's larger axis.
	// @param screen   The larger viewport dimension.
	// @param current  The scale in force, so a step down needs a whole step.
	inline uint32_t SurfaceScale(uint32_t authored, float coverage, uint32_t screen, uint32_t current) {
		if (authored == 0 || screen == 0 || !(coverage > 0.0f)) {
			return std::max(current, 1u);
		}

		const auto needed = static_cast<uint32_t>(std::ceil(coverage * static_cast<float>(screen)));

		// **The stop is "already at least the screen", not "the next step would
		// exceed it".** The second reads as safer and is why this did nothing at
		// all on the first run: a surface authored at more than half the viewport
		// could never take a step, which is every surface in a scene that sized
		// its panes sensibly. Overshooting the screen by one doubling costs texels
		// nobody samples; stopping short costs the sharpness this exists for.
		uint32_t scale = 1;
		while (authored * scale < needed && authored * scale < screen && scale < 16u) {
			scale *= 2u;
		}

		// **A step down costs a whole step, and it has to be measured against the
		// step below rather than against the one held.** A step *up* happens the
		// moment `needed` passes what the held size provides; measuring the step
		// down against half of that same number puts both thresholds on one
		// point, because a step is a factor of two and half of what is held is
		// exactly what the step below provides. That is a hysteresis one texel
		// wide - which is none.
		//
		// What it cost: a viewer standing near a pane, breathing on it, crossing
		// that single point every other frame. 32 reallocations in 60 frames from
		// a coverage wobble of 0.0008, each one releasing three render targets,
		// creating three more and clearing the slot's `Ready` - so the pane spent
		// those frames re-rendering from nothing, and showed its own flat tint on
		// any frame that re-render did not complete. The report was "a gray flash
		// a bunch when i walk around".
		//
		// A quarter of what is held is half of what the step below provides, so
		// the band between going up and coming down is a whole doubling wide, and
		// nothing a viewer does with their feet can cross it twice in a second.
		if (current > scale && needed * 4u > authored * current) {
			return current;
		}

		return scale;
	}
}
