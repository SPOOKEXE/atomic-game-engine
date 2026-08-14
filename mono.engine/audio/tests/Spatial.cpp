#include <engine/audio/Graph.hpp>
#include <engine/audio/Spatial.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_SUITE_ID("engine.audio.spatial")
TEST_DEPENDS("engine.audio.graph")

using engine::audio::DistanceBetween;
using engine::audio::DistanceGain;
using engine::audio::EmitterPlacement;
using engine::audio::ListenerPose;
using engine::audio::PanGain;
using engine::audio::Place;
using engine::audio::StereoGain;

namespace {
	bool Near(float value, float expected, float tolerance = 0.0001f) {
		return std::abs(value - expected) <= tolerance;
	}

	// A listener at the origin, facing -Z with +X to its right - the engine's
	// default and the arrangement every case below reasons about.
	ListenerPose AtOrigin() {
		return ListenerPose{};
	}

	EmitterPlacement At(float x, float y, float z) {
		EmitterPlacement placement;
		placement.X = x;
		placement.Y = y;
		placement.Z = z;
		return placement;
	}
}

TEST_CASE("panning keeps its power constant", "[audio][spatial]") {
	// **The reason it is not linear.** A linear pan drops about 3 dB in the
	// middle, so a sound swept across the front sags as it passes the centre.
	for (float pan = -1.0f; pan <= 1.0f; pan += 0.1f) {
		const StereoGain gain = PanGain(pan);
		const float power = gain.Left * gain.Left + gain.Right * gain.Right;
		INFO("pan " << pan);
		REQUIRE(Near(power, 1.0f, 0.001f));
	}
}

TEST_CASE("panning reaches each side and centres between them", "[audio][spatial]") {
	const StereoGain left = PanGain(-1.0f);
	CHECK(Near(left.Left, 1.0f));
	CHECK(Near(left.Right, 0.0f));

	const StereoGain right = PanGain(1.0f);
	CHECK(Near(right.Left, 0.0f));
	CHECK(Near(right.Right, 1.0f));

	const StereoGain centre = PanGain(0.0f);
	CHECK(Near(centre.Left, centre.Right));
	// Not 0.5 - that is the linear answer and the one that sags.
	CHECK(Near(centre.Left, 0.70710678f));
}

TEST_CASE("panning past the extremes is clamped", "[audio][spatial]") {
	CHECK(Near(PanGain(-5.0f).Left, PanGain(-1.0f).Left));
	CHECK(Near(PanGain(5.0f).Right, PanGain(1.0f).Right));
}

// --- distance --------------------------------------------------------------

TEST_CASE("inside the full-volume radius nothing is attenuated", "[audio][spatial]") {
	const EmitterPlacement placement = At(0, 0, 0);
	CHECK(DistanceGain(0.0f, placement) == 1.0f);
	CHECK(DistanceGain(placement.FalloffStart * 0.5f, placement) == 1.0f);
	CHECK(DistanceGain(placement.FalloffStart, placement) == 1.0f);
}

TEST_CASE("past the end it is exactly silent", "[audio][spatial]") {
	// Exactly zero rather than nearly. A tail that never quite reaches zero
	// keeps every sound in the world mixing for ever.
	const EmitterPlacement placement = At(0, 0, 0);
	CHECK(DistanceGain(placement.FalloffEnd, placement) == 0.0f);
	CHECK(DistanceGain(placement.FalloffEnd * 10.0f, placement) == 0.0f);
}

TEST_CASE("attenuation falls off monotonically between the two", "[audio][spatial]") {
	const EmitterPlacement placement = At(0, 0, 0);
	float previous = 1.0f;
	for (float distance = placement.FalloffStart; distance <= placement.FalloffEnd; distance += 1.0f) {
		const float gain = DistanceGain(distance, placement);
		INFO("distance " << distance);
		REQUIRE(gain <= previous);
		REQUIRE(gain >= 0.0f);
		REQUIRE(gain <= 1.0f);
		previous = gain;
	}
}

TEST_CASE("the falloff is steeper near the listener than a straight ramp", "[audio][spatial]") {
	// Sound intensity falls with the square of distance, and a linear ramp is
	// why a sound can seem to switch off as you walk away from it.
	EmitterPlacement placement = At(0, 0, 0);
	placement.FalloffStart = 1.0f;
	placement.FalloffEnd = 101.0f;

	const float halfway = DistanceGain(51.0f, placement);
	// A straight ramp would read 0.5 here. The inverse-square curve is well
	// below it.
	CHECK(halfway < 0.5f);
	CHECK(halfway > 0.0f);
}

TEST_CASE("a reversed or degenerate falloff does not misbehave", "[audio][spatial]") {
	// Somebody will type these in.
	EmitterPlacement reversed = At(0, 0, 0);
	reversed.FalloffStart = 50.0f;
	reversed.FalloffEnd = 5.0f;
	CHECK(DistanceGain(10.0f, reversed) >= 0.0f);
	CHECK(DistanceGain(10.0f, reversed) <= 1.0f);

	EmitterPlacement zero = At(0, 0, 0);
	zero.FalloffStart = 0.0f;
	zero.FalloffEnd = 0.0f;
	CHECK(DistanceGain(0.0f, zero) == 1.0f);
	CHECK(DistanceGain(1.0f, zero) == 0.0f);

	EmitterPlacement negative = At(0, 0, 0);
	negative.FalloffStart = -10.0f;
	negative.FalloffEnd = -5.0f;
	CHECK(DistanceGain(1.0f, negative) >= 0.0f);
	CHECK(DistanceGain(1.0f, negative) <= 1.0f);
}

TEST_CASE("distance is the plain euclidean one", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	CHECK(Near(DistanceBetween(listener, At(3, 4, 0)), 5.0f));
	CHECK(Near(DistanceBetween(listener, At(0, 0, 0)), 0.0f));
}

// --- the two together ------------------------------------------------------

TEST_CASE("a sound to the right is louder on the right", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	const StereoGain gain = Place(listener, At(1, 0, 0));
	CHECK(gain.Right > gain.Left);
}

TEST_CASE("a sound to the left is louder on the left", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	const StereoGain gain = Place(listener, At(-1, 0, 0));
	CHECK(gain.Left > gain.Right);
}

TEST_CASE("a sound straight ahead is centred", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	const StereoGain gain = Place(listener, At(0, 0, -1));
	CHECK(Near(gain.Left, gain.Right));
}

TEST_CASE("a sound on top of the listener is centred rather than a divide by zero", "[audio][spatial]") {
	// The obvious crash, and the less obvious one: a direction that flips
	// wildly as somebody walks through the emitter.
	const ListenerPose listener = AtOrigin();
	const StereoGain gain = Place(listener, At(0, 0, 0));

	CHECK(std::isfinite(gain.Left));
	CHECK(std::isfinite(gain.Right));
	CHECK(Near(gain.Left, gain.Right));
	CHECK(gain.Left > 0.0f);
}

TEST_CASE("turning the listener moves the sound to the other ear", "[audio][spatial]") {
	// The listener's own right vector is what panning reads, so rotating it
	// swaps the sides without the emitter moving.
	ListenerPose listener = AtOrigin();
	const EmitterPlacement placement = At(1, 0, 0);

	const StereoGain before = Place(listener, placement);
	CHECK(before.Right > before.Left);

	// Face the other way: right is now -X.
	listener.RightX = -1.0f;
	const StereoGain after = Place(listener, placement);
	CHECK(after.Left > after.Right);
}

TEST_CASE("a sound out of range is silent on both sides", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	EmitterPlacement placement = At(1000, 0, 0);
	const StereoGain gain = Place(listener, placement);

	CHECK(gain.Left == 0.0f);
	CHECK(gain.Right == 0.0f);
}

TEST_CASE("moving away is monotonically quieter", "[audio][spatial]") {
	const ListenerPose listener = AtOrigin();
	float previous = 2.0f;
	for (float z = 0.0f; z < 80.0f; z += 2.0f) {
		const StereoGain gain = Place(listener, At(0, 0, -z));
		const float total = gain.Left + gain.Right;
		INFO("distance " << z);
		REQUIRE(total <= previous + 0.0001f);
		previous = total;
	}
	CHECK(previous == 0.0f);
}
