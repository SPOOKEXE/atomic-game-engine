#include "PresentationPacing.hpp"

#include <engine/render/PresentationSchedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.presentation-pacing")
TEST_DEPENDS("engine.render.presentation-schedule")

using engine::render::PresentationSchedule;
using namespace std::chrono_literals;

TEST_CASE("the default client has a presentation opportunity on every update", "[client][presentation]") {
	const client::Options settings;
	REQUIRE(settings.Uncapped);
	PresentationSchedule schedule;
	schedule.SetRate(client::PresentationRateOf(settings));
	const PresentationSchedule::TimePoint start;
	REQUIRE(schedule.Due(start));
	schedule.Consume(start);
	CHECK(schedule.Due(start + 1ns));
	CHECK(schedule.Remaining(start + 1ns) == 0ns);
}

TEST_CASE(
	"an explicit FPS cap schedules presentation independently of simulation", "[client][presentation]"
) {
	client::Options settings;
	settings.MaximumFrameRate = 100;
	PresentationSchedule schedule;
	schedule.SetRate(client::PresentationRateOf(settings));
	const PresentationSchedule::TimePoint start;
	REQUIRE(schedule.Due(start));
	schedule.Consume(start);
	CHECK_FALSE(schedule.Due(start + 9ms));
	CHECK(schedule.Due(start + 10ms));
	CHECK(settings.TickRate == 60.0);

	settings.Uncapped = false;
	schedule.SetRate(client::PresentationRateOf(settings));
	CHECK(schedule.Rate() == 0);
	CHECK(schedule.Due(start + 1ns));
}

TEST_CASE(
	"headless service hosts retain a bounded cadence with uncapped defaults", "[client][presentation]"
) {
	for (const bool presentationHost : {false, true}) {
		CAPTURE(presentationHost);
		client::Options settings;
		settings.Headless = true;
		settings.DataFactory = !presentationHost;
		settings.PresentationSession = presentationHost ? 1 : 0;
		PresentationSchedule schedule;
		schedule.SetRate(client::PresentationRateOf(settings));
		REQUIRE(schedule.Rate() == 60);
		const PresentationSchedule::TimePoint start;
		REQUIRE(schedule.Due(start));
		schedule.Consume(start);
		CHECK_FALSE(schedule.Due(start + 1ms));
		CHECK(schedule.Due(start + 17ms));

		settings.MaximumFrameRate = 240;
		CHECK(client::PresentationRateOf(settings) == 240);
		settings.Uncapped = false;
		CHECK(client::PresentationRateOf(settings) == 240);
	}

	client::Options ordinaryHeadless;
	ordinaryHeadless.Headless = true;
	CHECK(client::PresentationRateOf(ordinaryHeadless) == 0);
}
