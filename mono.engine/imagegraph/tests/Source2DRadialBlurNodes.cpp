#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_radial_blur")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Uniform() {
		return imagegraph_test::MakeImage(2, 2, {64, 0, 0, 128, 64, 0, 0, 128, 64, 0, 0, 128, 64, 0, 0, 128});
	}
}
TEST_CASE(
	"Radial Blur retains source alpha normalization and unweighted alpha accumulation",
	"[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	for (const bool fade : {false, true}) {
		const auto run = RunNode(
			"pc.blur_radial",
			{{"surface_in", &image}},
			{{"strength", 1.},
			 {"center", Vector2{0, 0}},
			 {"center_unit", EnumValue{0}},
			 {"fade_distance", fade},
			 {"oversample", EnumValue{3}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			CHECK(run.Output().Pixels[pixel * 4] == 128);
			CHECK(run.Output().Pixels[pixel * 4 + 3] == (fade ? 255 : 128));
		}
	}
	const auto gamma = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 1.},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"gamma_correction", true},
		 {"oversample", EnumValue{3}}}
	);
	INFO(gamma.Message);
	REQUIRE(gamma.Ok);
	CHECK(
		gamma.Output().Pixels[0] ==
		detail::Quantize(std::pow(std::pow(64 / 255., 2.2) / (128 / 255.), 1 / 2.2))
	);
}
TEST_CASE(
	"Radial Blur retains fractional loop start and refuses empty fade weight", "[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	const auto half = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", .5},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(half.Message);
	REQUIRE(half.Ok);
	CHECK(half.Output().Pixels[0] == 128);
	const auto faded = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", .5},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"fade_distance", true},
		 {"oversample", EnumValue{3}}}
	);
	CHECK(faded.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Radial Blur spectral gradient contributes RGB while retaining source alpha", "[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	Gradient green;
	green.Keys = {{0, Colour{0, 255, 0, 0}}};
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 1.},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"colorize", EnumValue{2}},
		 {"gradient", green},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[1] == 64);
	CHECK(run.Output().Pixels[3] == 128);
}
TEST_CASE("Radial Blur safe greyscale draw precedes shader-only controls", "[imagegraph][source_2d]") {
	Image image;
	image.Width = 1;
	image.Height = 1;
	image.Format = SurfaceFormat::R8Unorm;
	image.Pixels = {64};
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 0.},
		 {"fade_distance", true},
		 {"colorize", EnumValue{99}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE("Radial Blur bounds work and diagnoses undefined selected samples", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const auto oversized = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 1e9}, {"center", Vector2{0, 0}}, {"center_unit", EnumValue{0}}}
	);
	CHECK(oversized.Code == Status::LimitExceeded);
	const Image empty = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 0});
	const auto transparent = RunNode(
		"pc.blur_radial",
		{{"surface_in", &empty}},
		{{"strength", 1.}, {"center", Vector2{0, 0}}, {"center_unit", EnumValue{0}}}
	);
	CHECK(transparent.Code == Status::UnsupportedExecution);
	const Image pixel = imagegraph_test::MakeImage(1, 1, {64, 0, 0, 255});
	const auto center = RunNode("pc.blur_radial", {{"surface_in", &pixel}}, {{"strength", 1.}});
	CHECK(center.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Radial Blur vectorizes authored controls and preserves the prior array on refusal",
	"[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	Node strengths{"strengths", "pc.array", "", {}, {}, {}};
	strengths.DynamicInputs = {{"input_0", ValueType::Scalar, .5}, {"input_1", ValueType::Scalar, 1.}};
	document.Nodes = {
		strengths,
		{"source",
		 "pc.checker",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"color_1", Colour{64, 0, 0, 128}},
		  {"color_2", Colour{64, 0, 0, 128}}}},
		{"radial",
		 "pc.blur_radial",
		 "",
		 {},
		 {{"center", Vector2{0, 0}}, {"center_unit", EnumValue{0}}, {"oversample", EnumValue{3}}}}
	};
	document.Links = {
		{"source", "surface_out", "radial", "surface_in"}, {"strengths", "array", "radial", "strength"}
	};
	document.Outputs = {{"blur", "radial", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	auto status = EvaluateArray(document, plan, "blur", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(
		images.Images[0].Pixels ==
		std::vector<uint8_t>{128, 0, 0, 128, 128, 0, 0, 128, 128, 0, 0, 128, 128, 0, 0, 128}
	);
	CHECK(images.Images[1].Pixels == images.Images[0].Pixels);
	const auto previous = images;
	document.Nodes.back().Values.push_back({"fade_distance", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateArray(document, plan, "blur", {}, images, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(images.Images.size() == previous.Images.size());
	for (size_t i = 0; i < images.Images.size(); ++i) {
		CHECK(images.Images[i].Pixels == previous.Images[i].Pixels);
		CHECK(images.Images[i].Width == previous.Images[i].Width);
		CHECK(images.Images[i].Height == previous.Images[i].Height);
	}
}
TEST_CASE("Radial Blur maps UV by angular influence before processor channels", "[imagegraph][source_2d]") {
	const Image source =
		imagegraph_test::MakeImage(2, 2, {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 0, 255});
	const Image uv = imagegraph_test::MakeImage(1, 1, {255, 0, 0, 0});
	const Image strengthMap = imagegraph_test::MakeImage(1, 1, {255, 255, 255, 0});
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &source}, {"uv_map", &uv}, {"strength_map", &strengthMap}},
		{{"strength_mapped", true},
		 {"strength_map_range", Vector2{0, 1}},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 85);
	CHECK(run.Output().Pixels[1] == 170);
	CHECK(run.Output().Pixels[3] == 255);
	const auto red = RunNode(
		"pc.blur_radial",
		{{"surface_in", &source}, {"uv_map", &uv}},
		{{"strength", 1.},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"oversample", EnumValue{3}},
		 {"channel", int64_t{1}}}
	);
	INFO(red.Message);
	REQUIRE(red.Ok);
	CHECK(red.Output().Pixels[0] == 85);
	CHECK(red.Output().Pixels[1] == 0);
}

TEST_CASE("Radial Blur spectral mode retains the source Zucconi coefficients", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 1.},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"colorize", EnumValue{1}},
		 {"shift", .5},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 159);
	CHECK(run.Output().Pixels[1] == 54);
	CHECK(run.Output().Pixels[2] == 12);
	CHECK(run.Output().Pixels[3] == 128);
}
TEST_CASE(
	"Radial Blur strength curve weights RGB while alpha retains source accumulation",
	"[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	Curve half;
	half.Header = {0, 1, 0, 0, 0, 1};
	half.Anchors = {{0, 0, 0, .5, 0, 0}, {0, 0, 1, .5, 0, 0}};
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}},
		{{"strength", 1.},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"strength_curved", true},
		 {"strength_curve", half},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[3] == 255);
}

TEST_CASE("Radial Blur retains the source max-before-absolute negative range", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const Image black = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 0});
	const auto run = RunNode(
		"pc.blur_radial",
		{{"surface_in", &image}, {"strength_map", &black}},
		{{"strength_mapped", true},
		 {"strength_map_range", Vector2{-10, -5}},
		 {"center", Vector2{0, 0}},
		 {"center_unit", EnumValue{0}},
		 {"fade_distance", true},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[3] == 176);
}
