#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_grain")
using namespace engine::imagegraph;
TEST_CASE(
	"Grain retains the pinned brightness modes and RGB amounts reused by HSV", "[imagegraph][source_2d]"
) {
	const Image source{1, 1, {64, 128, 192, 128}, 0};
	const std::array<std::array<uint8_t, 4>, 4> expected{
		{{105, 168, 232, 128}, {74, 148, 222, 128}, {95, 148, 202, 128}, {77, 152, 204, 128}}
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto run = imagegraph_test::RunNode(
			"pc.grain",
			{{"surface_in", &source}},
			{{"seed", 17.}, {"brightness", .5}, {"red", .25}, {"blend_mode", EnumValue{mode}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>(expected[mode].begin(), expected[mode].end()));
	}
	const auto unused = imagegraph_test::RunNode(
		"pc.grain", {{"surface_in", &source}}, {{"seed", 17.}, {"hue", 1.}, {"saturation", 1.}, {"value", 1.}}
	);
	REQUIRE(unused.Ok);
	CHECK(unused.Output().Pixels == source.Pixels);
}
TEST_CASE("Grain mapping and brightness curves retain source operation order", "[imagegraph][source_2d]") {
	const Image source{1, 1, {64, 128, 192, 128}, 0}, map{1, 1, {255, 255, 255, 255}, 0};
	const auto scalar =
		imagegraph_test::RunNode("pc.grain", {{"surface_in", &source}}, {{"seed", 17.}, {"brightness", .5}});
	const auto mapped = imagegraph_test::RunNode(
		"pc.grain",
		{{"surface_in", &source}, {"brightness_map", &map}},
		{{"seed", 17.}, {"brightness_mapped", true}, {"brightness_map_range", Vector2{0, .5}}}
	);
	REQUIRE(scalar.Ok);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels == scalar.Output().Pixels);
	Curve zero;
	zero.Header = {0, 1, 0, 0, 0, 1};
	zero.Anchors = {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}};
	const auto curved = imagegraph_test::RunNode(
		"pc.grain",
		{{"surface_in", &source}},
		{{"seed", 17.}, {"brightness", 1.}, {"brightness_curved", true}, {"brightness_curve", zero}}
	);
	INFO(curved.Message);
	REQUIRE(curved.Ok);
	CHECK(curved.Output().Pixels == source.Pixels);
	const auto disabled = imagegraph_test::RunNode(
		"pc.grain", {{"surface_in", &source}}, {{"seed", 17.}, {"brightness", 1.}, {"channel", int64_t{0}}}
	);
	REQUIRE(disabled.Ok);
	CHECK(disabled.Output().Pixels == source.Pixels);
}
TEST_CASE(
	"Grain scalar safe draw bypasses its shader while preserving channel "
	"staging",
	"[imagegraph][source_2d]"
) {
	for (const auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		const auto desc = DescribeSurfaceFormat(format);
		REQUIRE(desc);
		Image source{1, 1, std::vector<uint8_t>(desc->BytesPerPixel), 0, format};
		REQUIRE(StoreSurfacePixel(source, 0, 0, {.25, 0, 0, 1}));
		const auto run = imagegraph_test::RunNode(
			"pc.grain",
			{{"surface_in", &source}},
			{{"attribute_color_depth", EnumValue{3}}, {"brightness", 1.}, {"red", 1.}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	}
}
TEST_CASE(
	"Grain bounds the portable GLSL curve storage and diagnoses undefined curve inputs",
	"[imagegraph][source_2d]"
) {
	const Image source{1, 1, {64, 128, 192, 255}, 0};
	Curve curve;
	curve.Header = {0, 1, 0, 0, 0, 1};
	for (size_t i = 0; i < 9; ++i)
		curve.Anchors.push_back({0, 0, double(i) / 8, 0, 0, 0});
	const auto bounded = imagegraph_test::RunNode(
		"pc.grain",
		{{"surface_in", &source}},
		{{"seed", 17.}, {"brightness", 1.}, {"brightness_curved", true}, {"brightness_curve", curve}}
	);
	REQUIRE(bounded.Ok);
	CHECK(bounded.Output().Pixels == source.Pixels);
	curve.Anchors.push_back({0, 0, 1, 0, 0, 0});
	const auto large = imagegraph_test::RunNode(
		"pc.grain",
		{{"surface_in", &source}},
		{{"seed", 17.}, {"brightness", 1.}, {"brightness_curved", true}, {"brightness_curve", curve}}
	);
	CHECK_FALSE(large.Ok);
	CHECK(large.Code == Status::UnsupportedExecution);
	CHECK(large.Port == "brightness_curve");
	curve.Anchors.resize(9);
	curve.Header[1] = 0;
	const auto undefined = imagegraph_test::RunNode(
		"pc.grain",
		{{"surface_in", &source}},
		{{"seed", 17.}, {"brightness_curved", true}, {"brightness_curve", curve}}
	);
	CHECK_FALSE(undefined.Ok);
	CHECK(undefined.Code == Status::UnsupportedExecution);
	CHECK(undefined.Port == "brightness_curve");
}
TEST_CASE(
	"Grain and Contrast Blur retain graph outputs atomically under a refused byte budget",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 128, 192, 255}}}},
		{"grain", "pc.grain", "", {}, {{"seed", 17.}}},
		{"blur", "pc.blur_contrast", "", {}, {{"size", 0.}}}
	};
	document.Links = {
		{"solid", "surface_out", "grain", "surface_in"}, {"grain", "surface_out", "blur", "surface_in"}
	};
	document.Outputs = {{"image", "blur", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	const auto status = Evaluate(document, plan, "image", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 128, 192, 255});
	const Image previous = output;
	CHECK(Evaluate(document, plan, "image", {}, output, diagnostic, 1) == Status::LimitExceeded);
	CHECK(output == previous);
}
