#include "../src/PixelOpsColorAdjust.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_color_adjust")

using engine::imagegraph::Image;
using engine::imagegraph::detail::ColorAdjustControls;
using engine::imagegraph::detail::ColorAdjustStatus;
using engine::imagegraph::detail::ColorAdjustSurface;

TEST_CASE("Color Adjust scalar defaults preserve RGBA8 pixels", "[imagegraph]") {
	const Image source{2, 1, {64, 128, 192, 200, 0, 0, 0, 0}, 0};
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	REQUIRE(ColorAdjustSurface(source, output, {}) == ColorAdjustStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
}

TEST_CASE("Color Adjust applies brightness then exposure before HSV", "[imagegraph]") {
	const Image source{1, 1, {64, 128, 192, 200}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	ColorAdjustControls control;
	control.Brightness = 0.25;
	control.Exposure = 0.5;
	control.Saturation = -1.0;
	REQUIRE(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{96, 96, 96, 200});
}

TEST_CASE("Color Adjust mask uses premultiplied channels and red for alpha", "[imagegraph]") {
	const Image source{1, 1, {0, 0, 0, 128}, 0};
	const Image mask{1, 1, {128, 255, 0, 128}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	ColorAdjustControls control;
	control.Brightness = 1.0;
	control.Alpha = 0.5;
	REQUIRE(ColorAdjustSurface(source, output, control, &mask) == ColorAdjustStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 128, 0, 112});
}

TEST_CASE("Color Adjust surface blend modes remain bounded and deterministic", "[imagegraph]") {
	const Image source{1, 1, {64, 128, 192, 255}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	ColorAdjustControls control;
	control.Blend = {255, 0, 0, 128};
	control.BlendAmount = 1.0;
	for (int64_t mode = 0; mode <= 12; mode++) {
		control.BlendMode = mode;
		REQUIRE(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::Ok);
		const std::vector<uint8_t> first = output.Pixels;
		REQUIRE(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::Ok);
		CHECK(output.Pixels == first);
	}
	control.BlendMode = 0;
	REQUIRE(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{160, 64, 96, 255});
}

TEST_CASE("Color Adjust HSV saturation and HSL lightness blends use their own mix path", "[imagegraph]") {
	Image output{1, 1, {0, 0, 0, 0}, 0};
	ColorAdjustControls control;
	control.BlendAmount = 1.0;
	control.Blend = {255, 0, 0, 255};
	control.BlendMode = 7;
	REQUIRE(
		ColorAdjustSurface(Image{1, 1, {128, 128, 128, 255}, 0}, output, control) == ColorAdjustStatus::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 0, 0, 255});
	control.Blend = {255, 255, 255, 255};
	control.BlendAmount = 0.5;
	control.BlendMode = 8;
	REQUIRE(ColorAdjustSurface(Image{1, 1, {0, 0, 0, 255}, 0}, output, control) == ColorAdjustStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
}

TEST_CASE("Color Adjust refuses mapped and palette branches without changing output", "[imagegraph]") {
	const Image source{1, 1, {64, 128, 192, 255}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	ColorAdjustControls control;
	control.MappedParameter = true;
	CHECK(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::UnsupportedControl);
	control.MappedParameter = false;
	control.PaletteInput = true;
	CHECK(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::UnsupportedControl);
	control.PaletteInput = false;
	control.BlendMode = 13;
	CHECK(ColorAdjustSurface(source, output, control) == ColorAdjustStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Color Adjust graph evaluates scalar Surface mode and diagnoses mapped controls", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	Node source;
	source.Id = "source";
	source.Type = "image.solid";
	source.Values = {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{64, 128, 192, 200}}};
	Node adjust;
	adjust.Id = "adjust";
	adjust.Type = "image.color_adjust";
	adjust.Values = {{"brightness", 0.25}, {"exposure", 0.5}, {"saturation", -1.0}};
	document.Nodes = {source, adjust};
	document.Links.push_back({"source", "image", "adjust", "image"});
	document.Outputs.push_back({"out", "adjust", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	REQUIRE(Evaluate(document, plan, "out", result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{96, 96, 96, 200});
	document.Nodes[1].Values.push_back({"mapped", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", result, diagnostic) == Status::UnsupportedExecution);
}
