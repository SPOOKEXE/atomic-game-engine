// Presentation deadline arithmetic, including the wait exposed to the host.

#include <engine/render/PresentationSchedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.presentation-schedule")

using engine::render::PresentationSchedule;

namespace {
	using namespace std::chrono_literals;

	PresentationSchedule::TimePoint At(std::chrono::nanoseconds offset) {
		return PresentationSchedule::TimePoint{} + offset;
	}
}

TEST_CASE("an unlimited schedule is due on every update", "[render][presentation]") {
	PresentationSchedule schedule;

	CHECK(schedule.Due(At(0ns)));
	schedule.Consume(At(0ns));
	CHECK(schedule.Due(At(1ns)));
}

TEST_CASE("a limited schedule presents immediately and on its boundary", "[render][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(100);

	REQUIRE(schedule.Due(At(1s)));
	schedule.Consume(At(1s));
	CHECK(schedule.Remaining(At(1004ms)) == 6ms);
	CHECK_FALSE(schedule.Due(At(1009ms)));
	CHECK(schedule.Due(At(1010ms)));
	CHECK(schedule.Remaining(At(1010ms)) == 0ns);
}

TEST_CASE("observing a due frame does not consume it", "[render][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(60);

	REQUIRE(schedule.Due(At(2s)));
	CHECK(schedule.Due(At(2s)));
	CHECK(schedule.Due(At(3s)));
}

TEST_CASE("late presentation drops missed slots rather than bursting", "[render][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(10);

	REQUIRE(schedule.Due(At(0ns)));
	schedule.Consume(At(450ms));
	CHECK_FALSE(schedule.Due(At(499ms)));
	CHECK(schedule.Due(At(500ms)));
}

TEST_CASE("changing rate restarts with an immediate opportunity", "[render][presentation]") {
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

TEST_CASE("a rate above the clock resolution still advances", "[render][presentation]") {
	PresentationSchedule schedule;
	schedule.SetRate(std::numeric_limits<uint32_t>::max());

	REQUIRE(schedule.Due(At(0ns)));
	schedule.Consume(At(0ns));
	CHECK_FALSE(schedule.Due(At(0ns)));
	CHECK(schedule.Due(At(1ns)));
}

TEST_CASE("unlimited and unstarted schedules have no wait", "[render][presentation]") {
	PresentationSchedule unlimited;
	CHECK(unlimited.Remaining(At(1s)) == 0ns);

	PresentationSchedule limited;
	limited.SetRate(60);
	CHECK(limited.Remaining(At(1s)) == 0ns);
}
