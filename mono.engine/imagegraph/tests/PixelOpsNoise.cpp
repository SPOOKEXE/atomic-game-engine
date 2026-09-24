#include "../src/PixelOpsNoise.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_noise")

using engine::imagegraph::Image;
using engine::imagegraph::detail::NoiseStatus;
using engine::imagegraph::detail::SimplexControls;
using engine::imagegraph::detail::SimplexGrey;
using engine::imagegraph::detail::SimplexKernel;

TEST_CASE("Simplex kernel has source hash origin value", "[imagegraph]") {
	CHECK(SimplexKernel({0.0, 0.0}, 0.0) == 0.6);
	SimplexControls controls;
	controls.Tile = false;
	controls.Position = {0.5, 0.5};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	REQUIRE(SimplexGrey(controls, output) == NoiseStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{153, 153, 153, 255});
}

TEST_CASE("Simplex tiled and untiled source paths have fixed pixels", "[imagegraph]") {
	SimplexControls controls;
	controls.Seed = 42.0;
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	REQUIRE(SimplexGrey(controls, output) == NoiseStatus::Ok);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{108, 108, 108, 255, 151, 151, 151, 255, 171, 171, 171, 255, 150, 150, 150, 255}
	);
	const std::vector<uint8_t> first = output.Pixels;
	REQUIRE(SimplexGrey(controls, output) == NoiseStatus::Ok);
	CHECK(output.Pixels == first);
	controls.Tile = false;
	REQUIRE(SimplexGrey(controls, output) == NoiseStatus::Ok);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{163, 163, 163, 255, 103, 103, 103, 255, 98, 98, 98, 255, 119, 119, 119, 255}
	);
}

TEST_CASE("Simplex octave counts observed in supplied projects are deterministic", "[imagegraph]") {
	SimplexControls controls;
	controls.Seed = 42.0;
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	struct Expected {
		int64_t Iterations;
		std::vector<uint8_t> Grey;
	};
	const std::vector<Expected> expected{
		{1, {108, 151, 171, 150}},
		{2, {123, 170, 160, 155}},
		{3, {126, 157, 155, 155}},
		{5, {133, 155, 153, 152}},
	};
	for (const Expected &fixture : expected) {
		controls.Iterations = fixture.Iterations;
		REQUIRE(SimplexGrey(controls, output) == NoiseStatus::Ok);
		for (size_t index = 0; index < fixture.Grey.size(); index++)
			CHECK(output.Pixels[index * 4] == fixture.Grey[index]);
	}
}

TEST_CASE("Simplex reports undefined octave and level controls", "[imagegraph]") {
	SimplexControls controls;
	Image output{1, 1, {7, 7, 7, 7}, 0};
	controls.IterationAmplitude = 1.0;
	CHECK(SimplexGrey(controls, output) == NoiseStatus::UndefinedDivision);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	controls.IterationAmplitude = 0.5;
	controls.Scale.X = 0.0;
	CHECK(SimplexGrey(controls, output) == NoiseStatus::UndefinedDivision);
	controls.Scale.X = 0.25;
	controls.LevelIn = {0.5, 0.5};
	CHECK(SimplexGrey(controls, output) == NoiseStatus::UndefinedDivision);
	controls.Iterations = 17;
	CHECK(SimplexGrey(controls, output) == NoiseStatus::InvalidControl);
}

TEST_CASE("Simplex graph evaluates grayscale tile and diagnoses color modes", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	Node node;
	node.Id = "noise";
	node.Type = "image.noise_simplex";
	node.Values = {{"width", int64_t{2}}, {"height", int64_t{2}}, {"seed", 42.0}};
	document.Nodes.push_back(node);
	document.Outputs.push_back({"out", "noise", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{108, 108, 108, 255, 151, 151, 151, 255, 171, 171, 171, 255, 150, 150, 150, 255}
	);
	document.Nodes[0].Values.push_back({"color_mode", int64_t{1}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "color_mode");
}
