#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_normalize")
using namespace engine::imagegraph;
TEST_CASE(
	"Normalize preserves the source shared RGB range and fixed local BW mode", "[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {64, 64, 64, 255, 192, 192, 192, 255});
	for (int64_t channels = 0; channels <= 1; ++channels) {
		const auto global = imagegraph_test::RunNode(
			"pc.normalize", {{"surface_in", &image}}, {{"channels", EnumValue{channels}}}
		);
		INFO(global.Message);
		REQUIRE(global.Ok);
		CHECK(global.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 255});
		const auto local = imagegraph_test::RunNode(
			"pc.normalize",
			{{"surface_in", &image}},
			{{"channels", EnumValue{channels}}, {"modes", EnumValue{1}}, {"radius", int64_t{1}}}
		);
		INFO(local.Message);
		REQUIRE(local.Ok);
		CHECK(local.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 255});
	}
}
TEST_CASE(
	"Normalize retains transparent source range sentinels and diagnoses undefined reads",
	"[imagegraph][source_2d]"
) {
	const Image empty = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 0});
	const auto transparent = imagegraph_test::RunNode("pc.normalize", {{"surface_in", &empty}});
	INFO(transparent.Message);
	REQUIRE(transparent.Ok);
	CHECK(transparent.Output().Pixels == std::vector<uint8_t>{128, 128, 128, 0});
	const Image uniform = imagegraph_test::MakeImage(1, 1, {255, 255, 255, 255});
	CHECK(
		imagegraph_test::RunNode("pc.normalize", {{"surface_in", &uniform}}).Code ==
		Status::UnsupportedExecution
	);
	CHECK(
		imagegraph_test::RunNode(
			"pc.normalize", {{"surface_in", &uniform}}, {{"modes", EnumValue{1}}, {"radius", int64_t{0}}}
		).Code == Status::UnsupportedExecution
	);
	const Image red{1, 1, {128}, 0, SurfaceFormat::R8Unorm};
	CHECK(
		imagegraph_test::RunNode("pc.normalize", {{"surface_in", &red}}).Code == Status::UnsupportedExecution
	);
	const auto redLocal =
		imagegraph_test::RunNode("pc.normalize", {{"surface_in", &red}}, {{"modes", EnumValue{1}}});
	INFO(redLocal.Message);
	REQUIRE(redLocal.Ok);
	CHECK(redLocal.Output().Pixels == std::vector<uint8_t>{128});
}
