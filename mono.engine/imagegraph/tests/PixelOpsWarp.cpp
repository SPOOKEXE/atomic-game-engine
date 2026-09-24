#include "../src/PixelOpsWarp.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_warp")

using engine::imagegraph::Image;
using engine::imagegraph::detail::DisplaceControls;
using engine::imagegraph::detail::PolarControls;
using engine::imagegraph::detail::RenderDisplace;
using engine::imagegraph::detail::RenderPolar;
using engine::imagegraph::detail::WarpSampling;
using engine::imagegraph::detail::WarpStatus;

TEST_CASE("Displace linear samples a shifted source and clears outside", "[imagegraph]") {
	const Image source{3, 1, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255}, 0};
	const Image map{3, 1, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, 0};
	Image output{3, 1, std::vector<uint8_t>(12), 0};
	DisplaceControls control;
	control.MidValue = 0.0;
	REQUIRE(RenderDisplace(source, map, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{20, 0, 0, 255, 30, 0, 0, 255, 0, 0, 0, 0});
	const auto first = output.Pixels;
	REQUIRE(RenderDisplace(source, map, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == first);
}

TEST_CASE(
	"Displace linear uses alpha weighted luminance and filtered fractional coordinates", "[imagegraph]"
) {
	const Image source{2, 1, {0, 0, 0, 10, 100, 200, 50, 250}, 0};
	const Image map{2, 1, {255, 255, 255, 0, 255, 255, 255, 255}, 0};
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	DisplaceControls control;
	control.MidValue = 0.0;
	control.PositionPixels.X = 0.5;
	control.Sampling = WarpSampling::Linear;
	REQUIRE(RenderDisplace(source, map, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 10, 100, 200, 50, 250});
	const Image opaqueMap{2, 1, {255, 255, 255, 255, 255, 255, 255, 255}, 0};
	REQUIRE(RenderDisplace(source, opaqueMap, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{50, 100, 25, 130, 100, 200, 50, 250});
}

TEST_CASE("Displace gradient uses neighboring map brightness in UV units", "[imagegraph]") {
	const Image source{3, 1, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255}, 0};
	const Image map{3, 1, {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255}, 0};
	Image output{3, 1, std::vector<uint8_t>(12), 0};
	DisplaceControls control;
	control.Mode = 3;
	control.MidValue = 0.0;
	control.Strength = 1.0 / 3.0;
	REQUIRE(RenderDisplace(source, map, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{20, 0, 0, 255, 30, 0, 0, 255, 30, 0, 0, 255});
}

TEST_CASE("Displace gradient angle rotates the sampled direction", "[imagegraph]") {
	const Image source{
		3,
		3,
		{10, 0,	  0,  255, 10, 0,	0,	255, 10, 0,	  0,  255, 20, 0,	0,	255, 20, 0,
		 0,	 255, 20, 0,   0,  255, 30, 0,	 0,	 255, 30, 0,   0,  255, 30, 0,	 0,	 255},
		0
	};
	const Image map{
		3,
		3,
		{0,	  0,   0,	255, 255, 255, 255, 255, 255, 255, 255, 255, 0,	  0,   0,	255, 255, 255,
		 255, 255, 255, 255, 255, 255, 0,	0,	 0,	  255, 255, 255, 255, 255, 255, 255, 255, 255},
		0
	};
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	DisplaceControls control;
	control.Mode = 3;
	control.Strength = 1.0 / 3.0;
	control.MidValue = 0.0;
	control.AngleOffsetDegrees = 90.0;
	REQUIRE(RenderDisplace(source, map, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels[0] == 20);
	CHECK(output.Pixels[12] == 30);
	CHECK(output.Pixels[24] == 0);
	CHECK(output.Pixels[27] == 0);
}

TEST_CASE("Displace validates images and controls before writing", "[imagegraph]") {
	const Image source{1, 1, {10, 20, 30, 40}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	DisplaceControls control;
	control.Mode = 2;
	CHECK(RenderDisplace(source, source, output, control) == WarpStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	control.Mode = 0;
	const Image invalid{1, 1, {1, 2}, 0};
	CHECK(RenderDisplace(source, invalid, output, control) == WarpStatus::InvalidImage);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Polar direct transform follows source angle and tile equations", "[imagegraph]") {
	const Image source{2, 2, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	PolarControls control;
	control.Sampling = WarpSampling::Nearest;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{20, 0, 0, 255, 20, 0, 0, 255, 40, 0, 0, 255, 40, 0, 0, 255});
	control.Blend = 0.0;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	control.Blend = 1.0;
	control.Invert = true;
	control.Center.X = 0.4;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{30, 0, 0, 255, 30, 0, 0, 255, 10, 0, 0, 255, 10, 0, 0, 255});
	control.Invert = false;
	control.Center.X = 0.5;
	control.SwapAxis = true;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{30, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255, 40, 0, 0, 255});
	control.SwapAxis = false;
	control.Tile.X = 2.0;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{10, 0, 0, 255, 10, 0, 0, 255, 30, 0, 0, 255, 30, 0, 0, 255});
}

TEST_CASE("Polar filtered sampling and range failures are bounded", "[imagegraph]") {
	const Image source{2, 2, {0, 0, 0, 255, 100, 0, 0, 255, 0, 100, 0, 255, 100, 100, 0, 255}, 0};
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	PolarControls control;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels[0] == 50);
	CHECK(output.Pixels[1] == 25);
	const auto first = output.Pixels;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels == first);
	control.RangeDegrees = {30.0, 30.0};
	CHECK(RenderPolar(source, output, control) == WarpStatus::UndefinedRange);
	CHECK(output.Pixels == first);
}

TEST_CASE("Polar radius modes sample distinct exact pixels and reject logarithm origin", "[imagegraph]") {
	std::vector<uint8_t> pixels;
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x) {
			pixels.push_back(uint8_t((x + 1) * 10));
			pixels.insert(pixels.end(), {0, 0, 255});
		}
	const Image source{4, 4, pixels, 0};
	Image output{4, 4, std::vector<uint8_t>(64), 0};
	PolarControls control;
	control.Sampling = WarpSampling::Nearest;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels[20] == 20);
	control.RadiusMode = 1;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels[20] == 30);
	control.RadiusMode = 2;
	REQUIRE(RenderPolar(source, output, control) == WarpStatus::Ok);
	CHECK(output.Pixels[20] == 30);
	const auto previous = output.Pixels;
	control.Center = {0.375, 0.375};
	CHECK(RenderPolar(source, output, control) == WarpStatus::UndefinedRange);
	CHECK(output.Pixels == previous);
	control.RadiusMode = 3;
	CHECK(RenderPolar(source, output, control) == WarpStatus::InvalidControl);
}

TEST_CASE("Displace graph resolves reference position and rejects unsupported controls", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 5;
	Node source{
		"source",
		"image.gradient",
		"",
		{},
		{{"width", int64_t{2}},
		 {"height", int64_t{1}},
		 {"gradient", Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}}},
		 {"center", Vector2{1.0, 0.5}}}
	};
	Node map{
		"map",
		"image.solid",
		"",
		{},
		{{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}
	};
	Node warp{
		"warp",
		"image.displace",
		"",
		{},
		{{"mode", int64_t{0}}, {"mid_value", 0.0}, {"position", Vector2{0.5, 0.0}}}
	};
	document.Nodes = {source, map, warp};
	document.Links = {{"source", "image", "warp", "image"}, {"map", "image", "warp", "displace_map"}};
	document.Outputs = {{"out", "warp", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{191, 191, 191, 255, 0, 0, 0, 0});
	document.Nodes[2].Values.push_back({"oversample", int64_t{4}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "warp");
	document.Nodes[2].Values.pop_back();
	document.Links.pop_back();
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "displace_map");
}

TEST_CASE("Polar graph routes source pixels and diagnoses invalid radius mode", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 5;
	Node source{
		"source",
		"image.gradient",
		"",
		{},
		{{"width", int64_t{2}},
		 {"height", int64_t{1}},
		 {"gradient", Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}}},
		 {"center", Vector2{1.0, 0.5}}}
	};
	Node polar{"polar", "image.polar", "", {}, {{"blend", 0.0}}};
	document.Nodes = {source, polar};
	document.Links = {{"source", "image", "polar", "image"}};
	document.Outputs = {{"out", "polar", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 64, 64, 255, 191, 191, 191, 255});
	document.Nodes[1].Values.push_back({"radius_mode", int64_t{3}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "polar");
}
