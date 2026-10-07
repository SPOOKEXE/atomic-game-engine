#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_blend_depth")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Surface(uint32_t width, uint32_t height, std::vector<uint8_t> pixels) {
		return Image{width, height, std::move(pixels), 0};
	}
	Image PixelSurface(std::array<uint8_t, 4> pixel) {
		return Surface(1, 1, {pixel[0], pixel[1], pixel[2], pixel[3]});
	}
	std::array<uint8_t, 4> Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		const size_t offset = (size_t(y) * image.Width + x) * 4;
		return {
			image.Pixels[offset], image.Pixels[offset + 1], image.Pixels[offset + 2], image.Pixels[offset + 3]
		};
	}
	void CheckPixel(const Image &image, std::array<uint8_t, 4> expected, uint32_t x = 0, uint32_t y = 0) {
		CHECK(Pixel(image, x, y) == expected);
	}
	imagegraph_test::NodeRun
	Compare(const Image &first, const Image &second, int64_t mode, double depth1, double depth2) {
		return RunNode(
			"pc.blend_depth",
			{{"surface_1", &first}, {"surface_2", &second}},
			{{"depth_mode", EnumValue{mode}},
			 {"depth_range_1", Vector2{depth1, depth1}},
			 {"depth_range_2", Vector2{depth2, depth2}}}
		);
	}
	uint8_t Quantize(double value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
	}
}

TEST_CASE(
	"Blend Depth without Surface 2 samples Surface 1 and derives depth from transformed Y", "[blend_depth]"
) {
	auto first = Surface(1, 2, {20, 40, 60, 255, 80, 100, 120, 255});
	auto result = RunNode("pc.blend_depth", {{"surface_1", &first}});
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Width == 1);
	CHECK(result.Output().Height == 2);
	CHECK(result.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(result.Output("depth_out").Format == SurfaceFormat::RGBA8Unorm);
	CheckPixel(result.Output(), {20, 40, 60, 255}, 0, 0);
	CheckPixel(result.Output(), {80, 100, 120, 255}, 0, 1);
	CheckPixel(result.Output("depth_out"), {64, 64, 64, 255}, 0, 0);
	CheckPixel(result.Output("depth_out"), {191, 191, 191, 255}, 0, 1);
	auto unusedSecondTransform =
		RunNode("pc.blend_depth", {{"surface_1", &first}}, {{"scale_2", Vector2{0, 1}}});
	REQUIRE(unusedSecondTransform.Ok);
	CHECK(unusedSecondTransform.Output().Pixels == result.Output().Pixels);
}

TEST_CASE("Blend Depth implements all four strict and inclusive comparisons", "[blend_depth]") {
	const auto first = PixelSurface({255, 0, 0, 255});
	const auto second = PixelSurface({0, 0, 255, 255});
	for (const auto &[d1, d2] : {std::pair{.5, .5}, std::pair{.2, .8}, std::pair{.8, .2}}) {
		for (int64_t mode = 0; mode < 4; ++mode) {
			auto result = Compare(first, second, mode, d1, d2);
			INFO("mode " << mode << " depths " << d1 << " and " << d2 << ": " << result.Message);
			REQUIRE(result.Ok);
			const bool pass = mode == 0 ? d1 < d2 : mode == 1 ? d1 <= d2 : mode == 2 ? d1 > d2 : d1 >= d2;
			CheckPixel(
				result.Output(),
				pass ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 0, 255, 255}
			);
		}
	}
	const auto clampedMode = Compare(first, second, 9, .5, .5);
	REQUIRE(clampedMode.Ok);
	CheckPixel(clampedMode.Output(), {255, 0, 0, 255});
}

TEST_CASE("Blend Depth blends all four channels with foreground alpha squared", "[blend_depth]") {
	const auto first = PixelSurface({200, 100, 50, 128});
	const auto second = PixelSurface({40, 80, 120, 64});
	auto result = RunNode(
		"pc.blend_depth",
		{{"surface_1", &first}, {"surface_2", &second}},
		{{"depth_range_1", Vector2{0, 0}}, {"depth_range_2", Vector2{1, 1}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	CheckPixel(result.Output(), {120, 90, 85, 96});
	CheckPixel(result.Output("depth_out"), {0, 0, 0, 255});
}

TEST_CASE("Blend Depth reports the nearer depth in Greater modes", "[blend_depth]") {
	const auto first = PixelSurface({255, 0, 0, 255});
	const auto second = PixelSurface({0, 0, 255, 255});
	for (int64_t mode : {int64_t{2}, int64_t{3}}) {
		auto result = Compare(first, second, mode, .8, .2);
		INFO(result.Message);
		REQUIRE(result.Ok);
		CheckPixel(result.Output(), {255, 0, 0, 255});
		CheckPixel(result.Output("depth_out"), {51, 51, 51, 255});
	}
}

TEST_CASE("Blend Depth returns Surface 1 and its depth when Surface 2 is transparent", "[blend_depth]") {
	const auto first = PixelSurface({40, 80, 160, 128});
	const auto second = PixelSurface({255, 0, 0, 0});
	const auto depth1 = PixelSurface({0, 0, 255, 255});
	const auto depth2 = PixelSurface({0, 0, 0, 255});
	auto result = RunNode(
		"pc.blend_depth",
		{{"surface_1", &first}, {"depth_1", &depth1}, {"surface_2", &second}, {"depth_2", &depth2}},
		{{"depth_mode", EnumValue{0}}, {"depth_range_1", Vector2{0, 1}}, {"depth_range_2", Vector2{0, 1}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	CheckPixel(result.Output(), {40, 80, 160, 128});
	CheckPixel(result.Output("depth_out"), {85, 85, 85, 255});
}

TEST_CASE(
	"Blend Depth samples supplied depth as RGB mean times alpha and applies its range", "[blend_depth]"
) {
	const auto first = PixelSurface({255, 255, 255, 255});
	const auto depth = PixelSurface({255, 0, 0, 128});
	auto rgba = RunNode(
		"pc.blend_depth", {{"surface_1", &first}, {"depth_1", &depth}}, {{"depth_range_1", Vector2{0, .6}}}
	);
	INFO(rgba.Message);
	REQUIRE(rgba.Ok);
	CheckPixel(rgba.Output("depth_out"), {26, 26, 26, 255});

	const Image red{1, 1, {255}, 0, SurfaceFormat::R8Unorm};
	auto redDepth = RunNode(
		"pc.blend_depth", {{"surface_1", &first}, {"depth_1", &red}}, {{"depth_range_1", Vector2{0, .9}}}
	);
	INFO(redDepth.Message);
	REQUIRE(redDepth.Ok);
	CheckPixel(redDepth.Output("depth_out"), {77, 77, 77, 255});

	const auto nearestMap = Surface(2, 1, {0, 0, 0, 255, 255, 0, 0, 255});
	const auto single = PixelSurface({255, 255, 255, 255});
	auto nearest = RunNode(
		"pc.blend_depth",
		{{"surface_1", &single}, {"depth_1", &nearestMap}},
		{{"depth_range_1", Vector2{0, .9}}, {"interpolate", EnumValue{2}}}
	);
	INFO(nearest.Message);
	REQUIRE(nearest.Ok);
	CheckPixel(nearest.Output("depth_out"), {77, 77, 77, 255});
	const auto clampMap = Surface(2, 1, {255, 0, 0, 255, 0, 0, 0, 255});
	auto clamped = RunNode(
		"pc.blend_depth",
		{{"surface_1", &single}, {"depth_1", &clampMap}},
		{{"depth_range_1", Vector2{0, .9}},
		 {"position_1", Vector2{1, 0}},
		 {"position_1_unit", EnumValue{0}},
		 {"oversample", EnumValue{4}}}
	);
	INFO(clamped.Message);
	REQUIRE(clamped.Ok);
	CheckPixel(clamped.Output("depth_out"), {77, 77, 77, 255});
}

TEST_CASE("Blend Depth transforms UVs by pixel and reference position units", "[blend_depth]") {
	const auto first = Surface(2, 1, {255, 0, 0, 255, 0, 255, 0, 255});
	for (const auto &[position, unit] :
		 {std::pair{Vector2{1, 0}, int64_t{0}}, std::pair{Vector2{.5, 0}, int64_t{1}}}) {
		auto result = RunNode(
			"pc.blend_depth",
			{{"surface_1", &first}},
			{{"position_1", position}, {"position_1_unit", EnumValue{unit}}, {"oversample", EnumValue{1}}}
		);
		INFO(result.Message);
		REQUIRE(result.Ok);
		CheckPixel(result.Output(), {0, 0, 0, 0}, 0, 0);
		CheckPixel(result.Output(), {255, 0, 0, 255}, 1, 0);
	}
}

TEST_CASE("Blend Depth rotation and scale follow the shader row-vector transform", "[blend_depth]") {
	const auto first = Surface(2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255});
	auto rotated = RunNode("pc.blend_depth", {{"surface_1", &first}}, {{"rotation_1", 90.0}});
	INFO(rotated.Message);
	REQUIRE(rotated.Ok);
	CheckPixel(rotated.Output(), {0, 0, 255, 255}, 0, 0);
	CheckPixel(rotated.Output(), {255, 0, 0, 255}, 1, 0);
	CheckPixel(rotated.Output(), {255, 255, 0, 255}, 0, 1);
	CheckPixel(rotated.Output(), {0, 255, 0, 255}, 1, 1);

	const auto horizontal = Surface(4, 1, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255});
	auto scaled = RunNode("pc.blend_depth", {{"surface_1", &horizontal}}, {{"scale_1", Vector2{2, 1}}});
	INFO(scaled.Message);
	REQUIRE(scaled.Ok);
	CheckPixel(scaled.Output(), {0, 255, 0, 255}, 0, 0);
	CheckPixel(scaled.Output(), {0, 255, 0, 255}, 1, 0);
	CheckPixel(scaled.Output(), {0, 0, 255, 255}, 2, 0);
	CheckPixel(scaled.Output(), {0, 0, 255, 255}, 3, 0);
}

TEST_CASE("Blend Depth shares Surface 1 dimensions for Bicubic sampling other surfaces", "[blend_depth]") {
	const auto first = Surface(3, 1, {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
	const auto second = Surface(4, 1, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255});
	auto result = RunNode(
		"pc.blend_depth",
		{{"surface_1", &first}, {"surface_2", &second}},
		{{"depth_range_1", Vector2{1, 1}},
		 {"depth_range_2", Vector2{0, 0}},
		 {"position_2", Vector2{-1.0 / 12, 0}},
		 {"interpolate", EnumValue{3}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	CheckPixel(result.Output(), {255, 0, 0, 255});
}

TEST_CASE("Blend Depth loads every source format and clamps signed float output", "[blend_depth]") {
	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(1, 1, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image source{1, 1, std::vector<uint8_t>(layout->Bytes), 0, format};
		REQUIRE(StoreSurfacePixel(source, 0, 0, {.25, .5, .75, 1}));
		auto result = RunNode("pc.blend_depth", {{"surface_1", &source}});
		INFO("surface format " << int(format) << ": " << result.Message);
		REQUIRE(result.Ok);
		CHECK(result.Output().Format == SurfaceFormat::RGBA8Unorm);
		if (format == SurfaceFormat::R8Unorm || format == SurfaceFormat::R16Float ||
			format == SurfaceFormat::R32Float) {
			SurfacePixel loaded{};
			REQUIRE(LoadSurfacePixel(source, 0, 0, loaded));
			CheckPixel(result.Output(), {Quantize(loaded[0]), 0, 0, 255});
		} else {
			SurfacePixel loaded{};
			REQUIRE(LoadSurfacePixel(source, 0, 0, loaded));
			CheckPixel(
				result.Output(),
				{Quantize(loaded[0]), Quantize(loaded[1]), Quantize(loaded[2]), Quantize(loaded[3])}
			);
		}
	}
	Image signedFloat{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(signedFloat, 0, 0, {-.25, .5, 1.25, -.25}));
	auto clamped = RunNode("pc.blend_depth", {{"surface_1", &signedFloat}});
	INFO(clamped.Message);
	REQUIRE(clamped.Ok);
	CheckPixel(clamped.Output(), {0, 128, 255, 0});
}

TEST_CASE(
	"Blend Depth normalizes excessive enum values and names malformed transform refusals", "[blend_depth]"
) {
	const auto first = PixelSurface({255, 0, 0, 255});
	const auto malformedMode = RunNode("pc.blend_depth", {{"surface_1", &first}}, {{"depth_mode", 1.5}});
	CHECK_FALSE(malformedMode.Ok);
	CHECK(malformedMode.Port == "depth_mode");
	CHECK(malformedMode.Images.empty());
	auto zeroScale = RunNode("pc.blend_depth", {{"surface_1", &first}}, {{"scale_1", Vector2{0, 1}}});
	CHECK_FALSE(zeroScale.Ok);
	CHECK(zeroScale.Port == "scale_1");
	CHECK(zeroScale.Images.empty());
	const auto missing = RunNode("pc.blend_depth", {});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Port == "surface_1");
}

TEST_CASE("Blend Depth round-trips both outputs through grouped source arrays", "[blend_depth]") {
	Document document;
	document.FormatVersion = 9;
	document.Groups = {{"group", "depth group"}};
	document.Groups[0].Interpolation = 1;
	document.Groups[0].Oversample = 1;
	document.Junctions = {
		{"dimensions",
		 "group",
		 ValueType::Array,
		 ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{2, 1}}}}
	};
	document.Nodes = {
		{"solid", "pc.solid", "group", {}, {{"dimension_unit", EnumValue{0}}}},
		{"blend", "pc.blend_depth", "group", {}, {}}
	};
	document.Links = {
		{"dimensions", "value", "solid", "dimension"}, {"solid", "surface_out", "blend", "surface_1"}
	};
	document.Outputs = {{"colour", "blend", "surface_out"}, {"depth", "blend", "depth_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	for (const auto &[outputId, isDepth] : {std::pair{"colour", false}, std::pair{"depth", true}}) {
		ImageArray output;
		const auto status = EvaluateArray(restored, plan, outputId, {}, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Images.size() == 2);
		CHECK(output.Images[0].Width == 1);
		CHECK(output.Images[1].Width == 2);
		if (isDepth) {
			CheckPixel(output.Images[0], {128, 128, 128, 255});
			CheckPixel(output.Images[1], {128, 128, 128, 255});
		}
	}
}

TEST_CASE("Blend Depth applies animated position keys through both compiled outputs", "[blend_depth]") {
	Document document;
	document.FormatVersion = 9;
	document.Groups = {{"group", "depth group"}};
	document.Groups[0].Interpolation = 1;
	document.Groups[0].Oversample = 1;
	document.Nodes = {
		{"source", "image.captured", "group", {}, {{"source_id", std::string("pattern")}}},
		{"blend", "pc.blend_depth", "group", {}, {}}
	};
	document.Links = {{"source", "image", "blend", "surface_1"}};
	document.Outputs = {{"colour", "blend", "surface_out"}, {"depth", "blend", "depth_out"}};
	document.Keyframes = {
		{"blend", "position_1", 0, Vector2{0, 0}, "linear"},
		{"blend", "position_1", 2, Vector2{1, 0}, "linear"}
	};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{
		{{"pattern", Surface(2, 1, {255, 0, 0, 255, 0, 255, 0, 255})}}
	};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image initial;
	REQUIRE(Evaluate(restored, plan, "colour", request, initial, diagnostic) == Status::Ok);
	CheckPixel(initial, {255, 0, 0, 255}, 0, 0);
	CheckPixel(initial, {0, 255, 0, 255}, 1, 0);
	request.Tick = 1;
	Image animatedColour;
	REQUIRE(Evaluate(restored, plan, "colour", request, animatedColour, diagnostic) == Status::Ok);
	CheckPixel(animatedColour, {0, 0, 0, 0}, 0, 0);
	CheckPixel(animatedColour, {255, 0, 0, 255}, 1, 0);
	Image animatedDepth;
	REQUIRE(Evaluate(restored, plan, "depth", request, animatedDepth, diagnostic) == Status::Ok);
	CheckPixel(animatedDepth, {128, 128, 128, 255}, 0, 0);
	CheckPixel(animatedDepth, {128, 128, 128, 255}, 1, 0);
}

TEST_CASE("Blend Depth admits every source row before publishing compiled array outputs", "[blend_depth]") {
	Document document;
	document.FormatVersion = 9;
	document.Junctions = {
		{"dimensions",
		 "",
		 ValueType::Array,
		 ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{1024, 1024}}}}
	};
	document.Nodes = {
		{"solid", "pc.solid", "", {}, {{"dimension_unit", EnumValue{0}}}},
		{"blend", "pc.blend_depth", "", {}, {}}
	};
	document.Links = {
		{"dimensions", "value", "solid", "dimension"}, {"solid", "surface_out", "blend", "surface_1"}
	};
	document.Outputs = {{"out", "blend", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray output;
	output.Images = {{1, 1, {9, 8, 7, 6}, 0}};
	output.Items = {ImageArrayItem{size_t{0}}};
	const ImageArray before = output;
	const auto status = EvaluateArray(document, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "blend");
	CHECK(diagnostic.Port == "surface_1");
	CHECK(output.Images == before.Images);
	CHECK(output.Items == before.Items);
}

TEST_CASE("Blend Depth retains constant pixels through every inherited sampler", "[blend_depth]") {
	const auto first =
		Surface(2, 2, {64, 128, 192, 255, 64, 128, 192, 255, 64, 128, 192, 255, 64, 128, 192, 255});
	for (int64_t mode : {1, 2, 3, 4, 6}) {
		auto result = RunNode("pc.blend_depth", {{"surface_1", &first}}, {{"interpolate", EnumValue{mode}}});
		INFO(mode << ": " << result.Message);
		REQUIRE(result.Ok);
		CheckPixel(result.Output(), {64, 128, 192, 255});
	}
}
TEST_CASE("Blend Depth names malformed surface, Atlas and nonfinite controls", "[blend_depth]") {
	const auto first = PixelSurface({255, 0, 0, 255});
	Image broken{2, 1, {255, 0, 0, 255}, 0};
	auto malformed = RunNode("pc.blend_depth", {{"surface_1", &broken}});
	CHECK_FALSE(malformed.Ok);
	CHECK(malformed.Port == "surface_1");
	CHECK(malformed.Images.empty());
	AtlasValue atlas;
	atlas.Data.emplace().Surface.Data = first;
	atlas.Data->Kind = AtlasKind::SurfaceAtlas;
	auto refused = RunNode("pc.blend_depth", {}, {{"surface_1", atlas}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Port == "surface_1");
	CHECK(refused.Images.empty());
	for (const auto port : {"position_1", "anchor_1", "scale_1", "depth_range_1"}) {
		auto nonfinite = RunNode(
			"pc.blend_depth",
			{{"surface_1", &first}},
			{{port, Vector2{std::numeric_limits<double>::infinity(), 1}}}
		);
		CHECK_FALSE(nonfinite.Ok);
		CHECK(nonfinite.Port == port);
		CHECK(nonfinite.Images.empty());
	}
}
