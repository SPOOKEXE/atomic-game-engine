#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.source_kisrhombille")
using namespace engine::imagegraph;
TEST_CASE("Kisrhombille checker grouping follows the pinned cell geometry", "[imagegraph][source_2d]") {
	const std::array<std::array<std::string_view, 8>, 4> expected{
		{{"#..#.##.", ".#.#.##.", ".#..##.#", "######..", "#.##..#.", "#.#.#..#", ".##.#..#", "#..#.##."},
		 {".##.....", "..#.....", "..##...#", "..###.##", "..##...#", "..#.....", ".##.....", ".##....."},
		 {"....###.", "#...###.", "#....#..", "##......", "#....#..", "#...###.", "....###.", "....###."},
		 {"....###.", "#...###.", "#....#..", "##......", "#....#..", "#...###.", "....###.", "....###."}}
	};
	for (int64_t group = 0; group < 4; ++group) {
		const auto run = imagegraph_test::RunNode(
			"pc.kisrhombille",
			{},
			{{"dimension", Vector2{8, 8}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{2, 2}},
			 {"scale_unit", EnumValue{0}},
			 {"grouping", EnumValue{group}}}
		);
		INFO(run.Message);
		INFO(group);
		REQUIRE(run.Ok);
		REQUIRE(run.Output().Pixels.size() == 256);
		for (size_t y = 0; y < 8; ++y)
			for (size_t x = 0; x < 8; ++x) {
				for (size_t channel = 0; channel < 3; ++channel)
					CHECK(
						run.Output().Pixels[(y * 8 + x) * 4 + channel] ==
						(expected[group][y][x] == '#' ? 255 : 0)
					);
				CHECK(run.Output().Pixels[(y * 8 + x) * 4 + 3] == 255);
			}
	}
}
TEST_CASE(
	"Kisrhombille uses captured seed and retains UV alpha before the postmask", "[imagegraph][source_2d]"
) {
	const Image uv = imagegraph_test::MakeImage(1, 1, {128, 128, 0, 128});
	const Image mask = imagegraph_test::MakeImage(1, 1, {255, 0, 0, 128});
	const auto run = imagegraph_test::RunNode(
		"pc.kisrhombille",
		{{"uv_map", &uv}, {"mask", &mask}},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"render_type", EnumValue{1}},
		 {"seed", 12.5},
		 {"uv_mix", 0.},
		 {"colors", Gradient{0, {{0, {31, 47, 59, 255}}}}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	for (size_t index = 0; index < 4; ++index) {
		CHECK(run.Output().Pixels[index * 4] == 31);
		CHECK(run.Output().Pixels[index * 4 + 1] == 47);
		CHECK(run.Output().Pixels[index * 4 + 2] == 59);
		CHECK(run.Output().Pixels[index * 4 + 3] == 21);
	}
	auto refused = imagegraph_test::RunNode("pc.kisrhombille", {}, {{"render_type", EnumValue{1}}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Port == "seed");
	refused = imagegraph_test::RunNode("pc.kisrhombille", {}, {{"scale", Vector2{0, 1}}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Port == "scale");
}
TEST_CASE("Kisrhombille applies source rotation to output pixel centres", "[imagegraph][source_2d]") {
	const auto run = imagegraph_test::RunNode(
		"pc.kisrhombille",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{2, 2}},
		 {"scale_unit", EnumValue{0}},
		 {"angle", 90.},
		 {"color_1", Colour{17, 31, 47, 255}},
		 {"color_2", Colour{223, 199, 173, 255}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	// Source uses the row-vector product tx *= mat2(cos,-sin,sin,cos) on pixel-centre UVs.
	const size_t first = (0 * 8 + 0) * 4;
	const size_t second = (2 * 8 + 0) * 4;
	const std::array<uint8_t, 4> color1{17, 31, 47, 255}, color2{223, 199, 173, 255};
	for (size_t channel = 0; channel < 4; ++channel) {
		CHECK(run.Output().Pixels[first + channel] == color1[channel]);
		CHECK(run.Output().Pixels[second + channel] == color2[channel]);
	}
}
TEST_CASE("Kisrhombille position and scale alter shader cell selection", "[imagegraph][source_2d]") {
	const auto positioned = imagegraph_test::RunNode(
		"pc.kisrhombille",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{2, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{2, 2}},
		 {"scale_unit", EnumValue{0}},
		 {"color_1", Colour{17, 31, 47, 255}},
		 {"color_2", Colour{223, 199, 173, 255}}}
	);
	INFO(positioned.Message);
	REQUIRE(positioned.Ok);
	// Position is subtracted in normalized coordinates, so (2,0) samples the source field at (0,0).
	const size_t positionSample = (0 * 8 + 2) * 4;
	const std::array<uint8_t, 4> color2{223, 199, 173, 255};
	for (size_t channel = 0; channel < 4; ++channel)
		CHECK(positioned.Output().Pixels[positionSample + channel] == color2[channel]);

	const auto scaled = imagegraph_test::RunNode(
		"pc.kisrhombille",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{4, 2}},
		 {"scale_unit", EnumValue{0}},
		 {"color_1", Colour{17, 31, 47, 255}},
		 {"color_2", Colour{223, 199, 173, 255}}}
	);
	INFO(scaled.Message);
	REQUIRE(scaled.Ok);
	// At (0,0), scale (4,2) gives source shader coordinate (.125,.25), in cell color_1.
	const size_t scaleSample = 0;
	const std::array<uint8_t, 4> color1{17, 31, 47, 255};
	for (size_t channel = 0; channel < 4; ++channel)
		CHECK(scaled.Output().Pixels[scaleSample + channel] == color1[channel]);
}
