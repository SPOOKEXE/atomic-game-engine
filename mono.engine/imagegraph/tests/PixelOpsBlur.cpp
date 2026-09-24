#include "../src/PixelOpsBlur.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_blur")

using engine::imagegraph::Image;
using engine::imagegraph::detail::BlurStatus;
using engine::imagegraph::detail::GaussianBlurControls;
using engine::imagegraph::detail::GaussianBlurDefault;
using engine::imagegraph::detail::GaussianWeights;

TEST_CASE("Gaussian Blur source kernel is symmetric and normalized", "[imagegraph]") {
	const std::vector<double> weights = GaussianWeights(2);
	REQUIRE(weights.size() == 2);
	CHECK(std::abs(weights[0] - 0.401964) < 0.000001);
	CHECK(std::abs(weights[1] - 0.299018) < 0.000001);
	CHECK(std::abs(weights[0] + 2.0 * weights[1] - 1.0) < 1e-12);
}

TEST_CASE("Gaussian Blur uses two quantized alpha-weighted surface passes", "[imagegraph]") {
	Image source{3, 3, std::vector<uint8_t>(36), 0};
	source.Pixels[16] = 255;
	source.Pixels[19] = 255;
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	GaussianBlurControls control;
	control.Size = 2;
	REQUIRE(GaussianBlurDefault(source, output, control) == BlurStatus::Ok);
	for (size_t index = 0; index < output.Pixels.size(); index += 4) {
		CHECK(output.Pixels[index] == 255);
		CHECK(output.Pixels[index + 1] == 0);
		CHECK(output.Pixels[index + 2] == 0);
	}
	CHECK(
		std::vector<uint8_t>{
			output.Pixels[3],
			output.Pixels[7],
			output.Pixels[11],
			output.Pixels[15],
			output.Pixels[19],
			output.Pixels[23],
			output.Pixels[27],
			output.Pixels[31],
			output.Pixels[35]
		} == std::vector<uint8_t>{23, 31, 23, 31, 41, 31, 23, 31, 23}
	);
	const std::vector<uint8_t> first = output.Pixels;
	REQUIRE(GaussianBlurDefault(source, output, control) == BlurStatus::Ok);
	CHECK(output.Pixels == first);
}

TEST_CASE("Gaussian Blur keeps heterogeneous color and transparent border", "[imagegraph]") {
	const Image source{2, 1, {0, 0, 0, 255, 255, 0, 0, 255}, 0};
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	GaussianBlurControls control;
	control.Size = 2;
	REQUIRE(GaussianBlurDefault(source, output, control) == BlurStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{109, 0, 0, 72, 146, 0, 0, 72});
}

TEST_CASE("Gaussian Blur reports unsupported custom controls", "[imagegraph]") {
	const Image source{1, 1, {10, 20, 30, 255}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	GaussianBlurControls control;
	control.Intensity = 1;
	CHECK(GaussianBlurDefault(source, output, control) == BlurStatus::UnsupportedControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	control.Intensity = 0;
	control.Size = 33;
	CHECK(GaussianBlurDefault(source, output, control) == BlurStatus::InvalidControl);
	control.Size = 10.34;
	CHECK(GaussianBlurDefault(source, output, control) == BlurStatus::UnsupportedControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Gaussian Blur carries gamma and override colour through both surface passes", "[imagegraph]") {
	const Image source{1, 1, {128, 64, 32, 255}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	GaussianBlurControls control;
	control.Size = 1;
	control.GammaCorrection = true;
	REQUIRE(GaussianBlurDefault(source, output, control) == BlurStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	control.OverrideColor = true;
	control.OverrideColour = {20, 40, 60, 128};
	REQUIRE(GaussianBlurDefault(source, output, control) == BlurStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{20, 40, 60, 64});
}

TEST_CASE("Blur graph evaluates Gaussian default and refuses fractional source size", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	Node source;
	source.Id = "source";
	source.Type = "image.solid";
	source.Values = {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{128, 64, 32, 255}}};
	Node blur;
	blur.Id = "blur";
	blur.Type = "image.blur";
	blur.Values = {{"size", 1.0}, {"override_color", true}, {"color", Colour{20, 40, 60, 128}}};
	document.Nodes = {source, blur};
	document.Links.push_back({"source", "image", "blur", "image"});
	document.Outputs.push_back({"out", "blur", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	REQUIRE(Evaluate(document, plan, "out", result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{20, 40, 60, 64});
	document.Nodes[1].Values[0].Data = 10.34;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "size");
}
