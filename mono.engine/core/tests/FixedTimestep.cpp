#include <engine/core/FixedTimestep.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.fixedtimestep")

using Catch::Approx;
using engine::core::FixedTimestep;

TEST_CASE("the delta is the rate inverted and never varies", "[timestep]") {
	FixedTimestep timestep(60.0);

	REQUIRE(timestep.Rate() == Approx(60.0));
	REQUIRE(timestep.Delta() == Approx(1.0f / 60.0f));

	// Whatever the frame did, the tick delta is the same. That is the whole
	// point — a system integrating by it produces the same result at any
	// frame rate.
	timestep.Advance(0.100f);
	REQUIRE(timestep.Delta() == Approx(1.0f / 60.0f));
}

TEST_CASE("a rate of zero or less is refused", "[timestep]") {
	// Would divide to infinity, then run the clamp every frame forever.
	REQUIRE(FixedTimestep(0.0).Rate() > 0.0);
	REQUIRE(FixedTimestep(-30.0).Rate() > 0.0);
}

TEST_CASE("a frame shorter than a tick runs no tick", "[timestep]") {
	FixedTimestep timestep(60.0);

	// 300 fps against a 60 Hz tick. Four frames in five advance nothing, and
	// that is correct rather than a stall.
	REQUIRE(timestep.Advance(1.0f / 300.0f) == 0);
	REQUIRE(timestep.TotalTicks() == 0);
}

TEST_CASE("leftover time accumulates until it is a tick", "[timestep]") {
	FixedTimestep timestep(60.0);
	const float frame = 1.0f / 300.0f;

	int total = 0;
	for (int index = 0; index < 5; index++) {
		total += timestep.Advance(frame);
	}

	// Five frames of 1/300 is 1/60 exactly.
	REQUIRE(total == 1);
	REQUIRE(timestep.TotalTicks() == 1);
}

TEST_CASE("a slow frame runs several ticks", "[timestep]") {
	FixedTimestep timestep(60.0);

	// 20 fps against a 60 Hz tick.
	REQUIRE(timestep.Advance(1.0f / 20.0f) == 3);
	REQUIRE(timestep.TotalTicks() == 3);
}

TEST_CASE("the tick rate holds across render rates", "[timestep]") {
	// RENDER_PIPELINE.md §14's measurement, as a test rather than a note:
	// ticks per second must hold at 60 whatever the frame rate is.
	for (const double fps : { 30.0, 60.0, 144.0, 300.0, 600.0 }) {
		FixedTimestep timestep(60.0);
		const auto frame = static_cast<float>(1.0 / fps);

		// One simulated second.
		for (int index = 0; index < static_cast<int>(fps); index++) {
			timestep.Advance(frame);
		}

		REQUIRE(timestep.TotalTicks() >= 59);
		REQUIRE(timestep.TotalTicks() <= 60);
		REQUIRE(timestep.Dropped() == 0);
	}
}

TEST_CASE("a long stall is dropped, not carried", "[timestep]") {
	FixedTimestep timestep(60.0);

	// Two seconds of stall is 120 ticks owed. Running them would take longer
	// than a frame, arriving at the next frame owing even more — the death
	// spiral. Giving up is visible and recoverable; the spiral is neither.
	const int ticks = timestep.Advance(2.0f);
	REQUIRE(ticks == FixedTimestep::MAXIMUM_TICKS_PER_FRAME);
	REQUIRE(timestep.Dropped() > 100);

	// And the frame after a stall is back to normal, which is the property
	// carrying the remainder would destroy.
	REQUIRE(timestep.Advance(1.0f / 60.0f) == 1);
}

TEST_CASE("alpha is where the render sits between ticks", "[timestep]") {
	FixedTimestep timestep(60.0);

	timestep.Advance(1.0f / 60.0f);
	REQUIRE(timestep.Alpha() == Approx(0.0f).margin(1e-3));

	// Half a tick further on.
	timestep.Advance(1.0f / 120.0f);
	REQUIRE(timestep.Alpha() == Approx(0.5f).margin(1e-3));
}

TEST_CASE("alpha stays in range", "[timestep]") {
	FixedTimestep timestep(60.0);

	for (const float frame : { 0.0f, 0.001f, 0.016f, 0.5f, 2.0f }) {
		timestep.Advance(frame);
		REQUIRE(timestep.Alpha() >= 0.0f);
		REQUIRE(timestep.Alpha() <= 1.0f);
	}
}

TEST_CASE("a negative or zero frame time advances nothing", "[timestep]") {
	FixedTimestep timestep(60.0);

	REQUIRE(timestep.Advance(0.0f) == 0);
	REQUIRE(timestep.Advance(-1.0f) == 0);
	REQUIRE(timestep.TotalTicks() == 0);
}

TEST_CASE("changing the rate keeps the accumulated time", "[timestep]") {
	FixedTimestep timestep(60.0);
	timestep.Advance(1.0f / 120.0f);

	// Half a 60 Hz tick is a whole 120 Hz one. Discarding the accumulator on a
	// rate change would silently lose it.
	timestep.SetRate(120.0);
	REQUIRE(timestep.Advance(0.0f) == 1);
}

TEST_CASE("Reset clears everything", "[timestep]") {
	FixedTimestep timestep(60.0);
	timestep.Advance(2.0f);

	timestep.Reset();
	REQUIRE(timestep.TotalTicks() == 0);
	REQUIRE(timestep.Dropped() == 0);
	REQUIRE(timestep.Alpha() == Approx(0.0f));
}
