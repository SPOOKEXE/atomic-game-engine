// Presentation cadence without sleeping the update loop.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/PresentationSchedule.hpp>
#include <limits>

TEST_SUITE_ID("client.presentation-schedule")

using client::PresentationSchedule;

namespace {
	using namespace std::chrono_literals;

	PresentationSchedule::TimePoint At(std::chrono::nanoseconds offset) {
		return PresentationSchedule::TimePoint{} + offset;
	}
}

TEST_CASE("an unlimited schedule is due on every update", "[client][presentation]") {
	PresentationSchedule schedule;

	CHECK(schedule.Due(At(0ns)));
	schedule.Consume(At(0ns));
	CHECK(schedule.Due(At(1ns)));
}

TEST_CASE("a limited schedule presents immediately and on its boundary", "[client][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(100);

	REQUIRE(schedule.Due(At(1s)));
	schedule.Consume(At(1s));
	CHECK_FALSE(schedule.Due(At(1009ms)));
	CHECK(schedule.Due(At(1010ms)));
}

TEST_CASE("observing a due frame does not consume it", "[client][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(60);

	REQUIRE(schedule.Due(At(2s)));
	CHECK(schedule.Due(At(2s)));
	CHECK(schedule.Due(At(3s)));
}

TEST_CASE("late presentation drops missed slots rather than bursting", "[client][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(10);

	REQUIRE(schedule.Due(At(0ns)));
	schedule.Consume(At(450ms));
	CHECK_FALSE(schedule.Due(At(499ms)));
	CHECK(schedule.Due(At(500ms)));
}

TEST_CASE("changing rate restarts with an immediate opportunity", "[client][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(30);
	REQUIRE(schedule.Due(At(0ns)));
	schedule.Consume(At(0ns));
	REQUIRE_FALSE(schedule.Due(At(1ms)));

	schedule.SetRate(120);
	CHECK(schedule.Due(At(1ms)));

	schedule.Consume(At(1ms));
	REQUIRE_FALSE(schedule.Due(At(2ms)));
	schedule.SetRate(120);
	CHECK(schedule.Due(At(2ms)));
}

TEST_CASE("a rate above the clock resolution still advances", "[client][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(std::numeric_limits<uint32_t>::max());

	REQUIRE(schedule.Due(At(0ns)));
	schedule.Consume(At(0ns));
	CHECK_FALSE(schedule.Due(At(0ns)));
	CHECK(schedule.Due(At(1ns)));
}
