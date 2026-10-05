#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_hilbert")
using namespace engine::imagegraph;
TEST_CASE(
	"Hilbert source ordering uses opaque path RGB with pixel-centre line coverage", "[imagegraph][source_2d]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.hilbert",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"iteration", int64_t{1}},
		 {"thickness", 2.},
		 {"bg_color", Colour{7, 8, 9, 17}},
		 {"path_color", Gradient{0, {{0, {255, 255, 255, 0}}}}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const std::array<std::string_view, 8> rows{
		"........", "..####..", ".######.", ".##..##.", ".##..##.", ".##..##.", "........", "........"
	};
	REQUIRE(run.Output().Pixels.size() == 256);
	for (size_t y = 0; y < 8; ++y)
		for (size_t x = 0; x < 8; ++x) {
			const bool path = rows[y][x] == '#';
			for (size_t channel = 0; channel < 4; ++channel) {
				const std::array<uint8_t, 4> background{7, 8, 9, 17};
				CHECK(run.Output().Pixels[(y * 8 + x) * 4 + channel] == (path ? 255 : background[channel]));
			}
		}
}
TEST_CASE(
	"Hilbert preserves the source width-based vertical coordinates on nonsquare dimensions",
	"[imagegraph][source_2d]"
) {
	const auto run = imagegraph_test::RunNode(
		"pc.hilbert",
		{},
		{{"dimension", Vector2{8, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"iteration", int64_t{1}},
		 {"thickness", 2.}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const std::array<std::string_view, 4> rows{".######.", ".##..##.", ".##..##.", ".##..##."};
	REQUIRE(run.Output().Pixels.size() == 128);
	for (size_t y = 0; y < 4; ++y)
		for (size_t x = 0; x < 8; ++x)
			CHECK(run.Output().Pixels[(y * 8 + x) * 4] == (rows[y][x] == '#' ? 255 : 0));
}
TEST_CASE("Hilbert point expansion starts at the first expanded point", "[imagegraph][source_2d]") {
	const auto run = imagegraph_test::RunNode(
		"pc.hilbert",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"iteration", int64_t{2}},
		 {"thickness", 1.}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const std::array<std::string_view, 8> rows{
		".##..##.", "#.#.#.#.", "#.###.#.", "#.....#.", "###..##.", "..#.#...", ".##.###.", "........"
	};
	for (size_t y = 0; y < 8; ++y)
		for (size_t x = 0; x < 8; ++x)
			CHECK(run.Output().Pixels[(y * 8 + x) * 4] == (rows[y][x] == '#' ? 255 : 0));
}
TEST_CASE("Hilbert orientation choices follow the pinned point order", "[imagegraph][source_2d]") {
	const std::array<std::array<std::array<size_t, 2>, 2>, 4> pathSamples{{
		{{{4, 6}, {6, 4}}}, // L: right along the lower edge, then up the right edge.
		{{{2, 4}, {4, 2}}}, // T: up the left edge, then right along the top edge.
		{{{4, 2}, {2, 4}}}, // R: left along the top edge, then down the left edge.
		{{{6, 4}, {4, 6}}}	// B: down the right edge, then left along the lower edge.
	}};
	for (int64_t orientation = 0; orientation < 4; ++orientation) {
		const auto run = imagegraph_test::RunNode(
			"pc.hilbert",
			{},
			{{"dimension", Vector2{8, 8}},
			 {"dimension_unit", EnumValue{0}},
			 {"iteration", int64_t{1}},
			 {"orientation", EnumValue{orientation}},
			 {"thickness", 2.}}
		);
		INFO(run.Message);
		INFO(orientation);
		REQUIRE(run.Ok);
		for (const auto &[x, y] : pathSamples[size_t(orientation)])
			CHECK(run.Output().Pixels[(y * 8 + x) * 4] == 255);
	}
}
TEST_CASE("Hilbert shift advances cached gradient colors along the path", "[imagegraph][source_2d]") {
	const auto run = imagegraph_test::RunNode(
		"pc.hilbert",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"iteration", int64_t{1}},
		 {"orientation", EnumValue{1}},
		 {"thickness", 2.},
		 {"path_color", Gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}}},
		 {"shift", .25}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	// The source colors successive destinations at progress .5, .75, and 0 after wrapping.
	const std::array<std::array<size_t, 2>, 3> samples{{{{2, 4}}, {{4, 2}}, {{6, 4}}}};
	const std::array<std::array<uint8_t, 3>, 3> expected{{{{128, 0, 128}}, {{64, 0, 191}}, {{255, 0, 0}}}};
	for (size_t index = 0; index < samples.size(); ++index) {
		const auto [x, y] = samples[index];
		const size_t offset = (y * 8 + x) * 4;
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(run.Output().Pixels[offset + channel] == expected[index][channel]);
	}
}
TEST_CASE("Hilbert bounds recursion and follows source choice clamping", "[imagegraph][source_2d]") {
	auto run = imagegraph_test::RunNode("pc.hilbert", {}, {{"iteration", int64_t{11}}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::LimitExceeded);
	CHECK(run.Port == "iteration");
	run = imagegraph_test::RunNode("pc.hilbert", {}, {{"orientation", EnumValue{4}}});
	const auto lastOrientation = imagegraph_test::RunNode("pc.hilbert", {}, {{"orientation", EnumValue{3}}});
	REQUIRE(run.Ok);
	REQUIRE(lastOrientation.Ok);
	CHECK(run.Output().Pixels == lastOrientation.Output().Pixels);
	run = imagegraph_test::RunNode("pc.hilbert", {}, {{"shift", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::InvalidValue);
}
