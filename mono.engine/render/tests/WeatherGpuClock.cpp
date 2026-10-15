#include "WeatherGpuClock.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.weathergpuclock")

using namespace engine::render;

namespace {
	WeatherGpuClockSample Sample(uint64_t sourceTick = 100, uint64_t phaseTick = 100) {
		WeatherGpuClockSample sample;
		sample.SourceTick = sourceTick;
		sample.Time.Tick = sourceTick;
		sample.Time.PhaseTick = phaseTick;
		sample.Time.DisplacementQ8 = {12, 34, 56};
		sample.TickRate = 20;
		sample.Identity = 17;
		return sample;
	}

	WeatherGpuClock ReadyClock(const WeatherGpuClockSample &sample) {
		WeatherGpuClock clock;
		clock.Publication.Ready = true;
		clock.CommittedSample = sample;
		clock.AnchorTick = sample.SourceTick;
		return clock;
	}

	// Models submission bookkeeping without recording device commands.
	void Stage(WeatherGpuClock &clock, const WeatherGpuClockSample &sample, const void *command) {
		const auto plan = PlanWeatherGpuClock(clock, sample, command);
		REQUIRE(plan);
		REQUIRE(plan->Action != WeatherGpuClockAction::Reuse);
		clock.PendingSample = sample;
		clock.PendingAnchorTick = plan->AnchorTick;
		clock.Publication.Stage(1, command);
	}

	uint64_t PhaseOf(const WeatherGpuClockUniforms &uniforms) {
		return uint64_t(uniforms.Phase[0]) | (uint64_t(uniforms.Phase[1]) << 32);
	}
}

TEST_CASE("a new weather GPU clock anchors exact integer words", "[render][weather][clock]") {
	int command;
	WeatherGpuClock clock;
	const auto sample = Sample(71, (uint64_t(1) << 60) + 29);
	const auto plan = PlanWeatherGpuClock(clock, sample, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Anchor);
	CHECK(plan->AnchorTick == 71);
	CHECK(PhaseOf(plan->Uniforms) == sample.Time.PhaseTick);
	CHECK(plan->Uniforms.Phase[2] == 1);
	CHECK(plan->Uniforms.Phase[3] == 0);
	for (size_t axis = 0; axis < 3; ++axis)
		CHECK(plan->Uniforms.Time[axis] == sample.Time.DisplacementQ8[axis]);
	CHECK(plan->Uniforms.Time[3] == sample.Time.Revision);
	CHECK_FALSE(clock.Publication.Ready);
	CHECK_FALSE(clock.Publication.Pending);
}

TEST_CASE(
	"weather GPU clock advances carry phase and modular displacement exactly", "[render][weather][clock]"
) {
	int command;
	auto before = Sample(100, UINT32_MAX - 3ull);
	before.Time.DisplacementQ8 = {UINT32_MAX - 2, 1, 800};
	const auto clock = ReadyClock(before);
	auto after = Sample(101, uint64_t(UINT32_MAX) + 7);
	after.Time.DisplacementQ8 = {5, UINT32_MAX - 4, 799};
	const auto plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Advance);
	CHECK(plan->AnchorTick == before.SourceTick);
	CHECK(plan->Uniforms.Phase[2] == 0);
	CHECK(PhaseOf(plan->Uniforms) == 10);
	CHECK(before.Time.PhaseTick + PhaseOf(plan->Uniforms) == after.Time.PhaseTick);
	for (size_t axis = 0; axis < 3; ++axis)
		CHECK(
			uint32_t(before.Time.DisplacementQ8[axis] + plan->Uniforms.Time[axis]) ==
			after.Time.DisplacementQ8[axis]
		);
	CHECK(plan->Uniforms.Time[0] == 8);
	CHECK(plan->Uniforms.Time[1] == UINT32_MAX - 5);
	CHECK(plan->Uniforms.Time[2] == UINT32_MAX);
}

TEST_CASE(
	"weather GPU clock reuse ignores sub-code time but respects the anchor deadline",
	"[render][weather][clock]"
) {
	int command;
	const auto before = Sample();
	const auto clock = ReadyClock(before);
	auto after = before;
	after.SourceTick = 199;
	after.Time.PhaseFractionQ16 = 65535;
	after.Time.DisplacementRemainderQ12 = {4095, 0, 1};
	auto plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Reuse);
	after.SourceTick = 200;
	plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Anchor);
	CHECK(plan->AnchorTick == 200);
}

TEST_CASE(
	"weather GPU clock reanchors changed timelines and discontinuous seeks", "[render][weather][clock]"
) {
	int command;
	const auto before = Sample();
	const auto clock = ReadyClock(before);
	auto after = before;
	SECTION("identity") {
		++after.Identity;
	}
	SECTION("revision") {
		++after.Time.Revision;
	}
	SECTION("tick rate") {
		++after.TickRate;
	}
	SECTION("source backwards") {
		--after.SourceTick;
	}
	SECTION("phase backwards") {
		--after.Time.PhaseTick;
	}
	SECTION("large phase seek") {
		after.Time.PhaseTick += 41;
	}
	SECTION("maximum tick seek") {
		after.SourceTick = std::numeric_limits<uint64_t>::max();
		after.Time.PhaseTick = std::numeric_limits<uint64_t>::max();
	}
	const auto plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Anchor);
	CHECK(PhaseOf(plan->Uniforms) == after.Time.PhaseTick);
	CHECK(plan->AnchorTick == after.SourceTick);
}

TEST_CASE(
	"weather GPU clock phase gap and source anchor boundaries are inclusive", "[render][weather][clock]"
) {
	int command;
	const auto before = Sample();
	const auto clock = ReadyClock(before);
	auto after = Sample(141, 140);
	auto plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Advance);
	++after.Time.PhaseTick;
	plan = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(plan);
	CHECK(plan->Action == WeatherGpuClockAction::Anchor);
}

TEST_CASE("weather GPU clock pending samples are idempotent and command scoped", "[render][weather][clock]") {
	int command, otherCommand;
	const auto before = Sample();
	auto clock = ReadyClock(before);
	const auto after = Sample(101, 101);
	Stage(clock, after, &command);
	const auto repeat = PlanWeatherGpuClock(clock, after, &command);
	REQUIRE(repeat);
	CHECK(repeat->Action == WeatherGpuClockAction::Reuse);
	CHECK_FALSE(PlanWeatherGpuClock(clock, after, &otherCommand));
	CommitWeatherGpuClock(clock, &otherCommand);
	CHECK(clock.Publication.Pending);
	CHECK(clock.CommittedSample.Time.PhaseTick == before.Time.PhaseTick);
	CommitWeatherGpuClock(clock, &command);
	CHECK(clock.Publication.Ready);
	CHECK_FALSE(clock.Publication.Pending);
	CHECK(clock.CommittedSample.Time.PhaseTick == after.Time.PhaseTick);
	CHECK(clock.AnchorTick == before.SourceTick);
	CHECK(PlanWeatherGpuClock(clock, after, &otherCommand)->Action == WeatherGpuClockAction::Reuse);
}

TEST_CASE("weather GPU clock cancellation forces an absolute retry", "[render][weather][clock]") {
	int command, otherCommand;
	const auto before = Sample();
	auto clock = ReadyClock(before);
	const auto after = Sample(101, 101);
	Stage(clock, after, &command);
	DiscardWeatherGpuClock(clock, &otherCommand);
	CHECK(clock.Publication.Pending);
	CHECK(clock.Publication.Ready);
	DiscardWeatherGpuClock(clock, &command);
	CHECK_FALSE(clock.Publication.Pending);
	CHECK_FALSE(clock.Publication.Ready);
	CHECK(clock.CommittedSample.Time.PhaseTick == before.Time.PhaseTick);
	const auto retry = PlanWeatherGpuClock(clock, after, &otherCommand);
	REQUIRE(retry);
	CHECK(retry->Action == WeatherGpuClockAction::Anchor);
	CHECK(PhaseOf(retry->Uniforms) == after.Time.PhaseTick);
	Stage(clock, after, &otherCommand);
	CommitWeatherGpuClock(clock, &otherCommand);
	CHECK(clock.Publication.Ready);
	CHECK(clock.AnchorTick == after.SourceTick);
	Stage(clock, Sample(102, 102), &command);
	DiscardWeatherGpuClock(clock);
	CHECK_FALSE(clock.Publication.Ready);
	CHECK_FALSE(clock.Publication.Pending);
}

TEST_CASE("weather GPU clock planning refuses invalid commands and rates", "[render][weather][clock]") {
	int command;
	WeatherGpuClock clock;
	auto sample = Sample();
	CHECK_FALSE(PlanWeatherGpuClock(clock, sample, nullptr));
	sample.TickRate = 0;
	CHECK_FALSE(PlanWeatherGpuClock(clock, sample, &command));
	sample.TickRate = 121;
	CHECK_FALSE(PlanWeatherGpuClock(clock, sample, &command));
	sample.TickRate = 20;
	sample.Time.Revision = 0;
	CHECK_FALSE(PlanWeatherGpuClock(clock, sample, &command));
}
