#include "WeatherTemporal.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.weathertemporal")

namespace {
	using namespace engine::render;

	WeatherTemporalFrame Frame() {
		WeatherTemporalFrame frame;
		frame.Key.World = 31;
		frame.Key.Provider = 71;
		frame.Key.Content = 113;
		frame.Key.Epoch = 4;
		frame.Key.TimelineRevision = 8;
		frame.Key.Tick = 100;
		frame.Key.Width = 320;
		frame.Key.Height = 180;
		frame.Eye = {0, 10, 0};
		return frame;
	}
}

TEST_CASE("weather history accepts bounded camera and cloud movement", "[weather][temporal]") {
	const auto previous = Frame();
	auto current = previous;
	current.Eye.x = 1;
	current.Key.Tick++;
	current.Key.DisplacementQ8[0] = 20;
	CHECK(WeatherTemporalCompatible(previous, current));
	CHECK(WeatherCloudDisplacement(previous.Key, current.Key).x == 5.0f);
}

TEST_CASE("weather history rejects changed ownership and timeline", "[weather][temporal]") {
	const auto previous = Frame();
	for (unsigned change = 0; change < 12; ++change) {
		auto current = previous;
		switch (change) {
		case 0:
			current.Key.World++;
			break;
		case 1:
			current.Key.Provider++;
			break;
		case 2:
			current.Key.Content++;
			break;
		case 3:
			current.Key.Epoch++;
			break;
		case 4:
			current.Key.TimelineRevision++;
			break;
		case 5:
			current.Key.Width++;
			break;
		case 6:
			current.Key.Height++;
			break;
		case 7:
			current.Key.CameraCut = true;
			break;
		case 8:
			current.Key.Tick--;
			break;
		case 9:
			current.Key.Tick += 11;
			break;
		case 10:
			current.Key.CellSizeMetres *= 2;
			break;
		case 11:
			current.Key.TickRate *= 2;
			break;
		}
		CHECK_FALSE(WeatherTemporalCompatible(previous, current));
	}
	auto current = previous;
	current.Eye.x = 129;
	CHECK_FALSE(WeatherTemporalCompatible(previous, current));
	current = previous;
	current.Key.DisplacementQ8[0] = 513;
	CHECK_FALSE(WeatherTemporalCompatible(previous, current));
	current = previous;
	current.ViewProjection[0][0] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(WeatherTemporalCompatible(previous, current));
}

TEST_CASE("weather displacement remains continuous across integer wrapping", "[weather][temporal]") {
	auto previous = Frame();
	previous.Key.DisplacementQ8 = {UINT32_MAX - 9, 0, 3};
	auto current = previous;
	current.Key.DisplacementQ8 = {10, UINT32_MAX - 19, 3};
	const auto displacement = WeatherCloudDisplacement(previous.Key, current.Key);
	CHECK(displacement == glm::vec3(5, -5, 0));
	CHECK(WeatherTemporalCompatible(previous, current));
}

TEST_CASE("weather history publishes only its own submitted command", "[weather][temporal]") {
	WeatherTemporalHistory history;
	uint8_t firstCommand = 0;
	uint8_t otherCommand = 0;
	auto first = Frame();
	history.Stage(first, &firstCommand);
	history.Commit(&otherCommand);
	CHECK_FALSE(history.Ready);
	history.Commit(&firstCommand);
	REQUIRE(history.Ready);
	CHECK(history.Accepted.Key.Tick == 100);
	auto second = first;
	second.Key.Tick++;
	history.Stage(second, &otherCommand);
	history.Discard(&firstCommand);
	CHECK(history.PendingCommand == &otherCommand);
	history.Discard(&otherCommand);
	history.Commit(&otherCommand);
	CHECK_FALSE(history.Ready);
	CHECK(history.Accepted.Key.Tick == 100);
	CHECK(history.PendingCommand == nullptr);
}

TEST_CASE("weather reconstruction distinguishes occluder depths", "[weather][temporal]") {
	CHECK(WeatherDepthCompatible(10, 11));
	CHECK_FALSE(WeatherDepthCompatible(10, 20));
	CHECK(WeatherDepthCompatible(1000, 1016));
	CHECK_FALSE(WeatherDepthCompatible(1000, 1100));
	CHECK_FALSE(WeatherDepthCompatible(10, std::numeric_limits<float>::infinity()));
	CHECK_FALSE(WeatherDepthCompatible(-1, 1));
}

TEST_CASE("cloud-only reconstruction applies transmittance once", "[weather][temporal]") {
	const glm::vec4 scene{.8f, .4f, .2f, 1};
	CHECK(WeatherCloudComposite(scene, {0, 0, 0, 1}) == scene);
	CHECK(WeatherCloudComposite(scene, {.1f, .2f, .3f, 0}) == glm::vec4(.1f, .2f, .3f, 1));
	CHECK(WeatherCloudComposite(scene, {.1f, .2f, .3f, .5f}) == glm::vec4(.5f, .4f, .4f, 1));
}
