#include "../src/PixelOpsConversion.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_conversion")

using engine::imagegraph::Image;
using engine::imagegraph::detail::ConversionStatus;
using engine::imagegraph::detail::IdentityColorCurve;
using engine::imagegraph::detail::RenderGreyAlpha;
using engine::imagegraph::detail::RenderHsvExtract;
using engine::imagegraph::detail::RenderMonochrome;
using engine::imagegraph::detail::RenderRgbExtract;

namespace {
	std::array<Image, 4> FourOutputs(uint32_t width, uint32_t height) {
		std::array<Image, 4> output;
		for (Image &image : output) {
			image.Width = width;
			image.Height = height;
			image.Pixels.resize(size_t(width) * height * 4);
		}
		return output;
	}
}

TEST_CASE("Greyscale and BW use source luma, alpha and strict half threshold", "[imagegraph]") {
	const Image source{2, 2, {128, 64, 32, 128, 255, 255, 255, 128, 0, 0, 0, 0, 10, 20, 30, 40}, 0};
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	REQUIRE(RenderMonochrome(source, output, false, 0.0, 1.0) == ConversionStatus::Ok);
	CHECK(
		output.Pixels == std::vector<uint8_t>{38, 38, 38, 128, 128, 128, 128, 128, 0, 0, 0, 0, 3, 3, 3, 40}
	);
	REQUIRE(RenderMonochrome(source, output, true, 0.0, 1.0) == ConversionStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 128, 255, 255, 255, 128, 0, 0, 0, 0, 0, 0, 0, 40});
	const Image half{1, 1, {128, 128, 128, 255}, 0};
	Image halfOutput{1, 1, std::vector<uint8_t>(4), 0};
	REQUIRE(RenderMonochrome(half, halfOutput, true, 0.0, 1.0) == ConversionStatus::Ok);
	CHECK(halfOutput.Pixels[0] == 255);
	REQUIRE(RenderMonochrome(half, halfOutput, true, -0.1, 1.0) == ConversionStatus::Ok);
	CHECK(halfOutput.Pixels[0] == 0);
}

TEST_CASE("Grey to Alpha applies source luma times alpha and replacement", "[imagegraph]") {
	const Image source{1, 1, {128, 64, 32, 128}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	const auto identity = IdentityColorCurve();
	REQUIRE(
		RenderGreyAlpha(source, output, identity, false, true, {255, 255, 255, 255}) == ConversionStatus::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 38});
	REQUIRE(RenderGreyAlpha(source, output, identity, true, false, {0, 0, 0, 0}) == ConversionStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 64, 32, 217});
	REQUIRE(RenderGreyAlpha(source, output, identity, false, true, {5, 6, 7, 128}) == ConversionStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{5, 6, 7, 19});
}

TEST_CASE("RGBA Extract emits four exact named image payloads", "[imagegraph]") {
	const Image source{2, 2, {10, 20, 30, 40, 0, 0, 0, 0, 255, 128, 64, 255, 1, 2, 3, 4}, 0};
	auto output = FourOutputs(2, 2);
	REQUIRE(RenderRgbExtract(source, output, 0, false) == ConversionStatus::Ok);
	CHECK(
		std::vector<uint8_t>(output[0].Pixels.begin(), output[0].Pixels.begin() + 4) ==
		std::vector<uint8_t>{10, 0, 0, 255}
	);
	CHECK(
		std::vector<uint8_t>(output[1].Pixels.begin(), output[1].Pixels.begin() + 4) ==
		std::vector<uint8_t>{0, 20, 0, 255}
	);
	CHECK(
		std::vector<uint8_t>(output[2].Pixels.begin(), output[2].Pixels.begin() + 4) ==
		std::vector<uint8_t>{0, 0, 30, 255}
	);
	CHECK(
		std::vector<uint8_t>(output[3].Pixels.begin(), output[3].Pixels.begin() + 4) ==
		std::vector<uint8_t>{255, 255, 255, 40}
	);
	REQUIRE(RenderRgbExtract(source, output, 1, true) == ConversionStatus::Ok);
	CHECK(
		std::vector<uint8_t>(output[0].Pixels.begin(), output[0].Pixels.begin() + 4) ==
		std::vector<uint8_t>{10, 10, 10, 40}
	);
	CHECK(
		std::vector<uint8_t>(output[3].Pixels.begin(), output[3].Pixels.begin() + 4) ==
		std::vector<uint8_t>{40, 40, 40, 255}
	);
}

TEST_CASE("HSV Extract uses HSV H and S while Color Space selects V or HSL L", "[imagegraph]") {
	const Image source{2, 2, {255, 0, 0, 128, 0, 255, 0, 255, 0, 0, 255, 64, 0, 0, 0, 0}, 0};
	auto output = FourOutputs(2, 2);
	REQUIRE(RenderHsvExtract(source, output, 0) == ConversionStatus::Ok);
	CHECK(output[0].Pixels[0] == 0);
	CHECK(output[0].Pixels[4] == 85);
	CHECK(output[0].Pixels[8] == 170);
	CHECK(output[1].Pixels[0] == 255);
	CHECK(output[2].Pixels[0] == 255);
	CHECK(output[3].Pixels[3] == 128);
	REQUIRE(RenderHsvExtract(source, output, 1) == ConversionStatus::Ok);
	CHECK(output[0].Pixels[4] == 85);
	CHECK(output[1].Pixels[0] == 255);
	CHECK(output[2].Pixels[0] == 128);
	CHECK(output[2].Pixels[12] == 0);
}

TEST_CASE("Conversion controls fail before changing destination pixels", "[imagegraph]") {
	const Image source{1, 1, {128, 64, 32, 255}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	CHECK(RenderMonochrome(source, output, false, 2.0, 1.0) == ConversionStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	auto outputs = FourOutputs(1, 1);
	CHECK(RenderRgbExtract(source, outputs, 2, false) == ConversionStatus::InvalidControl);
	CHECK(RenderHsvExtract(source, outputs, 2) == ConversionStatus::InvalidControl);
}

TEST_CASE("Conversion graph routes each named output as an exact two by two image", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 128}}}},
		{"rgb", "image.rgb_extract", "", {}, {}},
		{"hsv", "image.hsv_extract", "", {}, {}}
	};
	document.Links = {{"source", "image", "rgb", "image"}, {"source", "image", "hsv", "image"}};
	document.Outputs = {
		{"red", "rgb", "red"},
		{"green", "rgb", "green"},
		{"blue", "rgb", "blue"},
		{"rgb_alpha", "rgb", "alpha"},
		{"hue", "hsv", "hue"},
		{"saturation", "hsv", "saturation"},
		{"value", "hsv", "value"},
		{"hsv_alpha", "hsv", "alpha"}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto checkPort = [&](const char *name, std::array<uint8_t, 4> pixel) {
		Image image;
		REQUIRE(Evaluate(document, plan, name, image, diagnostic) == Status::Ok);
		CHECK(image.Width == 2);
		CHECK(image.Height == 2);
		for (size_t offset = 0; offset < 16; offset += 4)
			CHECK(
				std::array<uint8_t, 4>{
					image.Pixels[offset],
					image.Pixels[offset + 1],
					image.Pixels[offset + 2],
					image.Pixels[offset + 3]
				} == pixel
			);
	};
	checkPort("red", {255, 0, 0, 255});
	checkPort("green", {0, 0, 0, 255});
	checkPort("blue", {0, 0, 0, 255});
	checkPort("rgb_alpha", {255, 255, 255, 128});
	checkPort("hue", {0, 0, 0, 128});
	checkPort("saturation", {255, 255, 255, 128});
	checkPort("value", {255, 255, 255, 128});
	checkPort("hsv_alpha", {255, 255, 255, 128});
	document.Nodes[1].Values.push_back({"output_array", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	CHECK(Evaluate(document, plan, "red", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "rgb");
}

TEST_CASE("Four conversion outputs refuse the evaluation byte budget before allocation", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3000}}, {"height", int64_t{3000}}, {"colour", Colour{1, 2, 3, 4}}}},
		{"rgb", "image.rgb_extract", "", {}, {}}
	};
	document.Links = {{"source", "image", "rgb", "image"}};
	document.Outputs = {{"out", "rgb", "red"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "rgb");
	CHECK(output.Pixels.empty());
}
