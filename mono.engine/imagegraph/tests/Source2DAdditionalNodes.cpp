#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_2d_additional_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("De-Stray preserves strictness and the pinned active-junction mapping", "[imagegraph][source_2d]") {
	const Image image =
		imagegraph_test::MakeImage(3, 3, {255, 0,	0, 255, 0,	 255, 0,   255, 255, 0,	  0, 255,
										  0,   255, 0, 255, 255, 255, 255, 255, 0,	 255, 0, 255,
										  255, 0,	0, 255, 0,	 255, 0,   255, 255, 0,	  0, 255});
	for (int64_t strict = 0; strict < 3; ++strict) {
		const auto run = RunNode(
			"pc.de_stray",
			{{"surface_in", &image}},
			{{"iteration", int64_t{1}}, {"strictness", EnumValue{strict}}}
		);
		REQUIRE(run.Ok);
		CHECK(
			engine::imagegraph::detail::ReadPixel(run.Output(), 1, 1) ==
			(strict == 2 ? engine::imagegraph::detail::Rgba{1, 1, 1, 1}
						 : engine::imagegraph::detail::Rgba{0, 1, 0, 1})
		);
	}
	const Image map = imagegraph_test::MakeImage(1, 1, {255, 255, 255, 255});
	const auto mapped = RunNode(
		"pc.de_stray",
		{{"surface_in", &image}, {"tolerance_map", &map}},
		{{"iteration", int64_t{1}},
		 {"strictness", EnumValue{1}},
		 {"tolerance_mapped", true},
		 {"tolerance_map_range", Vector2{0, 1}}}
	);
	REQUIRE(mapped.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(mapped.Output(), 1, 1) ==
		engine::imagegraph::detail::Rgba{0, 1, 0, 1}
	);
}

TEST_CASE(
	"Color Select preserves hue boundaries and ignores unused shader shifts", "[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 64, 128, 64, 64});
	const auto run = RunNode("pc.color_select", {{"surface_in", &image}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 255});
	const auto shifted = RunNode(
		"pc.color_select",
		{{"surface_in", &image}},
		{{"s_shift", .7}, {"v_shift", .8}, {"use_input_alpha", true}}
	);
	REQUIRE(shifted.Ok);
	CHECK(shifted.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 64});
}

TEST_CASE(
	"Round Corner preserves coordinate propagation and transparent cutouts", "[imagegraph][source_2d]"
) {
	const Image image =
		imagegraph_test::MakeImage(3, 3, {0, 0,	  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0,
										  0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	 0});
	const auto cut =
		RunNode("pc.corner", {{"surface_in", &image}}, {{"radius", int64_t{1}}, {"transparent", true}});
	REQUIRE(cut.Ok);
	CHECK(cut.Output().Pixels == std::vector<uint8_t>(36));
	const auto retained = RunNode(
		"pc.corner",
		{{"surface_in", &image}},
		{{"radius", int64_t{1}}, {"threshold", 0.0}, {"transparent", true}}
	);
	REQUIRE(retained.Ok);
	CHECK(retained.Output().Pixels == image.Pixels);
}

TEST_CASE(
	"Anisotropic Noise retains level transfer and UV alpha in both "
	"source modes",
	"[imagegraph][source_2d]"
) {
	const Image uv = imagegraph_test::MakeImage(1, 1, {128, 128, 0, 128});
	const auto waterfall = RunNode(
		"pc.noise_aniso",
		{{"uv_map", &uv}},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 3.5},
		 {"x_amount", 0.0},
		 {"render_mode", EnumValue{1}},
		 {"level_out", Vector2{.25, .75}}}
	);
	REQUIRE(waterfall.Ok);
	CHECK(waterfall.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 128, 64, 64, 64, 128});
	const auto blend = RunNode(
		"pc.noise_aniso",
		{},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 3.5},
		 {"seed_2", 1.25},
		 {"x_amount", 0.0},
		 {"y_amount", 0.0}}
	);
	REQUIRE(blend.Ok);
	CHECK(blend.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 255});
}
