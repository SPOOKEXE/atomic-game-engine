#include "../src/PixelOpsChannelAssembly.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_channel_assembly")

using engine::imagegraph::Colour;
using engine::imagegraph::Image;
using engine::imagegraph::detail::ChannelAssemblyStatus;
using engine::imagegraph::detail::RenderGammaMap;
using engine::imagegraph::detail::RenderHsvCombine;
using engine::imagegraph::detail::RenderMultiplyAlpha;
using engine::imagegraph::detail::RenderOverrideChannel;
using engine::imagegraph::detail::RenderRgbCombine;

TEST_CASE("RGB Combine copies named channels or alpha-weighted greyscale", "[imagegraph]") {
	const Image source{2, 2, {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 9, 18, 27, 0}, 0};
	Image output{2, 2, std::vector<uint8_t>(16), 0};
	const std::array<const Image *, 4> inputs{&source, &source, &source, &source};
	REQUIRE(RenderRgbCombine(inputs, output, 0, 0.0) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	REQUIRE(RenderRgbCombine(inputs, output, 1, 0.0) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{85, 85, 85, 85, 43, 43, 43, 43, 21, 21, 21, 21, 0, 0, 0, 0});
	const std::array<const Image *, 4> missing{&source, nullptr, nullptr, nullptr};
	REQUIRE(RenderRgbCombine(missing, output, 0, 0.25) == ChannelAssemblyStatus::Ok);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin(), output.Pixels.begin() + 4) ==
		std::vector<uint8_t>{255, 64, 64, 255}
	);
}

TEST_CASE("HSV Combine samples average RGB times alpha and converts HSV or HSL", "[imagegraph]") {
	const Image hue{1, 1, {0, 0, 0, 255}, 0};
	const Image saturation{1, 1, {255, 255, 255, 255}, 0};
	const Image value{1, 1, {255, 255, 255, 255}, 0};
	const Image alpha{1, 1, {255, 255, 255, 128}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	const std::array<const Image *, 4> inputs{&hue, &saturation, &value, &alpha};
	REQUIRE(RenderHsvCombine(inputs, output, 0) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 128});
	REQUIRE(RenderHsvCombine(inputs, output, 1) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 128});
	const std::array<const Image *, 4> absent{&hue, nullptr, &value, nullptr};
	REQUIRE(RenderHsvCombine(absent, output, 0) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}

TEST_CASE("Override Channel preserves unconnected channels and samples brightness", "[imagegraph]") {
	const Image base{1, 1, {10, 20, 30, 40}, 0};
	const Image replacement{1, 1, {90, 30, 0, 128}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	const std::array<const Image *, 4> inputs{nullptr, &replacement, nullptr, nullptr};
	REQUIRE(RenderOverrideChannel(base, inputs, output, 1) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{10, 30, 30, 40});
	REQUIRE(RenderOverrideChannel(base, inputs, output, 0) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{10, 20, 30, 40});
}

TEST_CASE(
	"Multiply Alpha clears threshold pixels and composites before forcing opaque alpha", "[imagegraph]"
) {
	const Image source{2, 1, {200, 100, 50, 128, 200, 100, 50, 127}, 0};
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	REQUIRE(RenderMultiplyAlpha(source, output, 0.5, Colour{128, 255, 0, 255}) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{150, 100, 25, 255, 0, 0, 0, 0});
}

TEST_CASE("Gamma Map applies pinned 2.2 exponent and preserves alpha", "[imagegraph]") {
	const Image source{1, 1, {64, 128, 192, 77}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	REQUIRE(RenderGammaMap(source, output, false) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{136, 186, 224, 77});
	REQUIRE(RenderGammaMap(source, output, true) == ChannelAssemblyStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{12, 56, 137, 77});
}

TEST_CASE("Channel assembly refuses dimensions, missing sources and invalid controls", "[imagegraph]") {
	const Image source{1, 1, {1, 2, 3, 4}, 0};
	const Image wrong{2, 1, std::vector<uint8_t>(8), 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	CHECK(
		RenderRgbCombine({nullptr, nullptr, nullptr, nullptr}, output, 0, 0.0) ==
		ChannelAssemblyStatus::MissingSource
	);
	CHECK(
		RenderRgbCombine({&wrong, nullptr, nullptr, nullptr}, output, 0, 0.0) ==
		ChannelAssemblyStatus::InvalidImage
	);
	CHECK(
		RenderRgbCombine({&source, nullptr, nullptr, nullptr}, output, 2, 0.0) ==
		ChannelAssemblyStatus::InvalidControl
	);
	CHECK(
		RenderHsvCombine({&source, nullptr, nullptr, nullptr}, output, 2) ==
		ChannelAssemblyStatus::InvalidControl
	);
	CHECK(
		RenderMultiplyAlpha(source, output, 2.0, {255, 255, 255, 255}) ==
		ChannelAssemblyStatus::InvalidControl
	);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Channel assembly graph routes RGB and HSV inputs through named image ports", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"red",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 128}}}},
		{"green",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 255, 0, 255}}}},
		{"black",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{0, 0, 0, 255}}}},
		{"white",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}},
		{"alpha",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 128}}}},
		{"rgb", "image.combine_rgb", "", {}, {}},
		{"hsv", "image.combine_hsv", "", {}, {}}
	};
	document.Links = {
		{"red", "image", "rgb", "red"},
		{"green", "image", "rgb", "green"},
		{"red", "image", "rgb", "alpha"},
		{"black", "image", "hsv", "hue"},
		{"white", "image", "hsv", "saturation"},
		{"white", "image", "hsv", "value"},
		{"alpha", "image", "hsv", "alpha"}
	};
	document.Outputs = {{"rgb", "rgb", "image"}, {"hsv", "hsv", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "rgb", output, diagnostic) == Status::Ok);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{255, 255, 0, 128, 255, 255, 0, 128, 255, 255, 0, 128, 255, 255, 0, 128}
	);
	REQUIRE(Evaluate(document, plan, "hsv", output, diagnostic) == Status::Ok);
	CHECK(
		output.Pixels == std::vector<uint8_t>{255, 0, 0, 128, 255, 0, 0, 128, 255, 0, 0, 128, 255, 0, 0, 128}
	);
	document.Nodes[5].Values.push_back({"array_input", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "rgb", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "array_input");
}

TEST_CASE("Override Gamma and Multiply Alpha graph nodes keep source-backed pixels", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{64, 128, 192, 77}}}},
		{"replacement",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{90, 30, 0, 128}}}},
		{"override", "image.override_channel", "", {}, {{"sampling_type", int64_t{1}}}},
		{"gamma", "image.gamma_map", "", {}, {}},
		{"multiply",
		 "image.multiply_alpha",
		 "",
		 {},
		 {{"threshold", 0.0}, {"bg_color", Colour{128, 255, 0, 255}}}}
	};
	document.Links = {
		{"source", "image", "override", "image"},
		{"replacement", "image", "override", "green"},
		{"source", "image", "gamma", "image"},
		{"source", "image", "multiply", "image"}
	};
	document.Outputs = {
		{"override", "override", "image"}, {"gamma", "gamma", "image"}, {"multiply", "multiply", "image"}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "override", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 30, 192, 77});
	REQUIRE(Evaluate(document, plan, "gamma", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{136, 186, 224, 77});
	REQUIRE(Evaluate(document, plan, "multiply", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{42, 128, 58, 255});
}
