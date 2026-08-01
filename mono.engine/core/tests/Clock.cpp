#include <engine/core/Clock.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <thread>

TEST_SUITE_ID("engine.core.clock")

using Catch::Approx;
using engine::core::Clock;
using engine::core::FrameClock;

TEST_CASE("the clock only moves forward", "[clock]") {
	// Monotonic is the whole contract. A wall clock corrected backwards
	// mid-frame would produce a negative delta, and every consumer of it
	// divides.
	uint64_t previous = Clock::Nanoseconds();
	for (int index = 0; index < 2'000; index++) {
		const uint64_t now = Clock::Nanoseconds();
		REQUIRE(now >= previous);
		previous = now;
	}
}

TEST_CASE("seconds and nanoseconds agree", "[clock]") {
	const double seconds = Clock::Seconds();
	const auto nanoseconds = static_cast<double>(Clock::Nanoseconds());

	REQUIRE(seconds == Approx(nanoseconds / 1e9).epsilon(0.001));
}

TEST_CASE("the first frame has no delta", "[clock]") {
	FrameClock clock;

	// There is no previous frame to have elapsed from. Reporting anything else
	// makes the first frame of every run an outlier in the statistics.
	REQUIRE(clock.Tick() == Approx(0.0f));
	REQUIRE(clock.Frame() == 0);
	REQUIRE(clock.Now() == Approx(0.0));
}

TEST_CASE("ticking advances the frame index and the clock", "[clock]") {
	FrameClock clock;
	clock.Tick();

	for (int index = 1; index <= 3; index++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		clock.Tick();
		REQUIRE(clock.Frame() == static_cast<uint64_t>(index));
	}

	REQUIRE(clock.Now() > 0.005);
	REQUIRE(clock.Delta() > 0.0f);
}

TEST_CASE("a long stall is clamped", "[clock]") {
	FrameClock clock;
	clock.Tick();

	// A breakpoint in a debugger is not a two-minute frame, and simulation code
	// should not have to defend against one.
	std::this_thread::sleep_for(
		std::chrono::milliseconds(static_cast<int>(FrameClock::MAXIMUM_DELTA * 1000.0f) + 60)
	);

	const float delta = clock.Tick();
	REQUIRE(delta == Approx(FrameClock::MAXIMUM_DELTA));
	REQUIRE(clock.Delta() == Approx(FrameClock::MAXIMUM_DELTA));
}

TEST_CASE("Now is unclamped even when a delta was", "[clock]") {
	FrameClock clock;
	clock.Tick();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	clock.Tick();

	// The clamp protects the delta a system integrates with. Elapsed time is a
	// measurement and must stay true, or the two disagree about how long the
	// run has been going.
	REQUIRE(clock.Now() >= 0.009);
}
