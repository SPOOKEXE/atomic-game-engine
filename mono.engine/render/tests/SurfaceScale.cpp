// How big a pane's texture is, and - the part that has a bug's name on it -
// how often that answer is allowed to change.
//
// **No device, which is why this is a suite at all.** The decision is arithmetic
// over four numbers and `Renderer::Render` needs a GPU to reach it, so the
// function moved into a header of its own rather than staying a lambda in the
// render path. `render/src/SurfaceScale.hpp` carries the argument.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SurfaceScale.hpp>
#include <cmath>

TEST_SUITE_ID("engine.render.surfacescale")

using engine::render::SurfaceScale;

namespace {
	// A pane authored at 1024 in a 1920 viewport, which is the shape every demo
	// scene in the repo has.
	constexpr uint32_t AUTHORED = 1024;
	constexpr uint32_t SCREEN = 1920;

	// Where the step up sits, as a fraction of the viewport's larger axis: the
	// scale doubles once the pane wants more texels than the authored size has.
	constexpr float THRESHOLD = static_cast<float>(AUTHORED) / static_cast<float>(SCREEN);
}

TEST_CASE("a pane's texture grows with the screen it covers and stops at both ends", "[render][surface]") {
	// **The authored size is the floor and the screen is the ceiling.** A scene
	// that asked for a large mirror keeps it however small the pane goes, and
	// nothing is served by rendering more texels than the pane can occupy.
	CHECK(SurfaceScale(AUTHORED, 0.05f, SCREEN, 1) == 1);
	CHECK(SurfaceScale(AUTHORED, 1.0f, SCREEN, 1) == 2);

	// A pane already authored at more than the viewport never steps: it has the
	// texels, they are just spread over less screen.
	CHECK(SurfaceScale(4096, 1.0f, SCREEN, 1) == 1);

	// No coverage is no information - a pane nothing can see keeps the size it
	// has rather than being shrunk to the floor and regrown on the frame the
	// viewer turns back round.
	CHECK(SurfaceScale(AUTHORED, 0.0f, SCREEN, 4) == 4);
}

TEST_CASE("a viewer breathing on a pane does not resize it every frame", "[render][surface]") {
	// **The bug this file was written for, and it was a hysteresis one texel
	// wide.** A step *up* happens when the pane wants more than the held size
	// provides; the step down used to be tested against half of that same
	// number - and a step is a factor of two, so half of what is held is exactly
	// what the step below provides. Both thresholds sat on one point.
	//
	// What crosses that point is a viewer standing still. A character bobs, a
	// camera arm settles, a hand moves a mouse - the pane's screen coverage
	// wobbles by a fraction of a percent, and every crossing released three
	// render targets and created three more. The slot's `Ready` is cleared by
	// that, so the pane spent those frames re-rendering from nothing and drew
	// its own flat tint on any frame the re-render did not complete.
	//
	// The report was "a gray flash a bunch when i walk around".
	uint32_t held = 1;
	size_t resizes = 0;

	for (int frame = 0; frame < 240; frame++) {
		// Parked on the threshold, wobbling by a thousandth of the screen.
		const float coverage = THRESHOLD + 0.001f * std::sin(static_cast<float>(frame) * 1.7f);

		const uint32_t next = SurfaceScale(AUTHORED, coverage, SCREEN, held);
		resizes += next != held ? 1u : 0u;
		held = next;
	}

	// One step up as the viewer arrives, and nothing after it.
	CHECK(resizes <= 1);
}

TEST_CASE("a pane that really is walked away from does come back down", "[render][surface]") {
	// **The other half, because refusing to shrink is also a bug** - it holds
	// four times the texels for a pane the size of a coin, per viewport, for as
	// long as the scene is open. The band is a whole doubling wide and no wider:
	// step up when the pane wants more than it holds, step down when it wants
	// less than a quarter of it.
	const uint32_t held = SurfaceScale(AUTHORED, 1.0f, SCREEN, 1);
	REQUIRE(held == 2);

	// Just inside the band, which is where the wobble above lives.
	CHECK(SurfaceScale(AUTHORED, THRESHOLD * 0.9f, SCREEN, held) == 2);

	// And out the far side of it.
	CHECK(SurfaceScale(AUTHORED, THRESHOLD * 0.2f, SCREEN, held) == 1);
}
