#include "../src/PixelOpsVignette.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_vignette")

using engine::imagegraph::Image;
using engine::imagegraph::detail::RenderVignetteScalar;
using engine::imagegraph::detail::VignetteControls;
using engine::imagegraph::detail::VignetteStatus;

TEST_CASE("Vignette scalar source darkens RGB and preserves alpha", "[imagegraph]") {
	const Image source{1, 1, {100, 80, 40, 71}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	VignetteControls controls;
	controls.Exposure = 0.0;
	REQUIRE(RenderVignetteScalar(source, output, controls) == VignetteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 71});
	controls.Strength = 0.0;
	REQUIRE(RenderVignetteScalar(source, output, controls) == VignetteStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	controls.Strength = 0.5;
	controls.Color = {0, 0, 0, 255};
	REQUIRE(RenderVignetteScalar(source, output, controls) == VignetteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{25, 20, 10, 71});
}

TEST_CASE("Vignette scalar Lighten branch follows reciprocal strength", "[imagegraph]") {
	const Image source{1, 1, {20, 10, 5, 200}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	VignetteControls controls;
	controls.Exposure = 0.0;
	controls.Strength = 0.5;
	controls.Lighten = 1.0;
	REQUIRE(RenderVignetteScalar(source, output, controls) == VignetteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{80, 40, 20, 200});
}

TEST_CASE("Vignette source coordinate equation is sampled at pixel centers", "[imagegraph]") {
	const Image source{
		3,
		3,
		{100, 0,  0,   99, 100, 0,	0,	 99, 100, 0,  0,   99, 100, 0,	0,	 99, 100, 0,
		 0,	  99, 100, 0,  0,	99, 100, 0,	 0,	  99, 100, 0,  0,	99, 100, 0,	 0,	  99},
		0
	};
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	REQUIRE(RenderVignetteScalar(source, output, {}) == VignetteStatus::Ok);
	CHECK(output.Pixels[0] == 73);
	CHECK(output.Pixels[16] == 98);
	CHECK(output.Pixels[32] == 73);
	CHECK(output.Pixels[3] == 99);
	const std::vector<uint8_t> first = output.Pixels;
	REQUIRE(RenderVignetteScalar(source, output, {}) == VignetteStatus::Ok);
	CHECK(output.Pixels == first);
}

TEST_CASE("Vignette Roundness warps coordinates toward Center", "[imagegraph]") {
	const Image source{1, 1, {100, 100, 100, 77}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	VignetteControls controls;
	controls.Center = {0.25, 0.5};
	controls.Roundness = 1.0;
	REQUIRE(RenderVignetteScalar(source, output, controls) == VignetteStatus::Ok);
	// Warped UV is (0.375, 0.5); the source raises 0.87890625 to 0.75.
	CHECK(output.Pixels == std::vector<uint8_t>{91, 91, 91, 77});
}

TEST_CASE("Vignette reports undefined power without fabricating a colour", "[imagegraph]") {
	const Image source{1, 1, {100, 80, 40, 71}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	VignetteControls controls;
	controls.Exposure = -1.0;
	CHECK(RenderVignetteScalar(source, output, controls) == VignetteStatus::UndefinedPower);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	controls.Exposure = 0.0;
	controls.Roundness = -0.5;
	CHECK(RenderVignetteScalar(source, output, controls) == VignetteStatus::UndefinedPower);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Vignette graph evaluates authored scalar controls and source-era Exponent no-op", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 4;
	Node source;
	source.Id = "source";
	source.Type = "image.solid";
	source.Values = {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{100, 80, 40, 71}}};
	Node vignette;
	vignette.Id = "vignette";
	vignette.Type = "image.vignette";
	vignette.Values = {{"exposure", 0.0}, {"exponent", 0.9}};
	document.Nodes = {source, vignette};
	document.Links.push_back({"source", "image", "vignette", "image"});
	document.Outputs.push_back({"out", "vignette", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 71});
	document.Nodes[1].Values[1].Data = 0.1;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 71});
	document.Nodes[1].Values[0].Data = -1.0;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 71});
}

TEST_CASE("Vignette graph diagnoses mapped inputs and missing surface", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	Node source{
		"source",
		"image.solid",
		"",
		{},
		{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}
	};
	Node vignette{"vignette", "image.vignette", "", {}, {}};
	document.Nodes = {source, vignette};
	document.Links = {
		{"source", "image", "vignette", "image"}, {"source", "image", "vignette", "roundness_map"}
	};
	document.Outputs.push_back({"out", "vignette", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "vignette");
	document.Links.clear();
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "image");
}
