// Blur family fixtures.

#include "../src/PixelOpsBlur.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.node_blur")

using namespace engine::imagegraph;
using imagegraph_test::MakeImage;
using imagegraph_test::RunNode;

namespace {
	Image Pattern(uint32_t width, uint32_t height) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (size_t index = 0; index < image.Pixels.size(); index++)
			image.Pixels[index] = static_cast<uint8_t>((index * 37 + index / 4 * 11) % 256);
		return image;
	}
}

TEST_CASE("Blur with Empty oversampling matches the native default Gaussian path", "[imagegraph]") {
	const Image source = Pattern(9, 7);
	for (const double size : {1.0, 3.0, 4.0, 8.0}) {
		INFO(size);
		const auto run = RunNode("pc.blur", {{"surface_in", &source}}, {{"size", size}, {"oversample", EnumValue{1}}});
		REQUIRE(run.Ok);
		Image reference{source.Width, source.Height, std::vector<uint8_t>(source.Pixels.size()), 0};
		detail::GaussianBlurControls controls;
		controls.Size = size;
		REQUIRE(detail::GaussianBlurDefault(source, reference, controls) == detail::BlurStatus::Ok);
		CHECK(run.Output().Pixels == reference.Pixels);
	}
}

TEST_CASE("Blur keeps flat colour under the default Repeat oversampling", "[imagegraph]") {
	std::vector<uint8_t> pixels;
	for (int index = 0; index < 20; index++)
		pixels.insert(pixels.end(), {70, 140, 210, 255});
	const Image flat = MakeImage(5, 4, pixels);
	const auto run = RunNode("pc.blur", {{"surface_in", &flat}}, {{"size", 3.0}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == flat.Pixels);
}

TEST_CASE("Shadow shifts, colours and composites under the surface", "[imagegraph]") {
	const Image source = MakeImage(3, 1, {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0});
	const auto run = RunNode(
		"pc.shadow",
		{{"surface_in", &source}},
		{{"grow", int64_t{0}}, {"blur", int64_t{0}}, {"shift", Vector2{1.0, 0.0}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 0, 128, 0, 0, 0, 0});
	CHECK(run.Output("shadow_only").Pixels == std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0});
}

TEST_CASE("Directional blur is exact at zero strength and keeps flat colour", "[imagegraph]") {
	std::vector<uint8_t> opaque;
	for (size_t index = 0; index < 5 * 3; index++)
		opaque.insert(opaque.end(), {static_cast<uint8_t>(index * 13), static_cast<uint8_t>(index * 7), 90, 255});
	const Image source = MakeImage(5, 3, opaque);
	const auto still = RunNode("pc.blur_directional", {{"surface_in", &source}}, {{"strength", 0.0}});
	REQUIRE(still.Ok);
	CHECK(still.Output().Pixels == source.Pixels);
	std::vector<uint8_t> pixels;
	for (int index = 0; index < 20; index++)
		pixels.insert(pixels.end(), {30, 60, 90, 255});
	const Image flat = MakeImage(5, 4, pixels);
	const auto run = RunNode("pc.blur_directional", {{"surface_in", &flat}}, {{"strength", 16.0}, {"direction", 30.0}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == flat.Pixels);
}

TEST_CASE("Zoom blur is exact at zero strength", "[imagegraph]") {
	std::vector<uint8_t> opaque;
	for (size_t index = 0; index < 4 * 4; index++)
		opaque.insert(opaque.end(), {static_cast<uint8_t>(index * 15), 40, static_cast<uint8_t>(250 - index * 9), 255});
	const Image source = MakeImage(4, 4, opaque);
	const auto run = RunNode("pc.blur_zoom", {{"surface_in", &source}}, {{"strength", 0.0}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == source.Pixels);
}

TEST_CASE("Bloom adds the tinted bright pass by Strength", "[imagegraph]") {
	// 0.502 luma passes the 0.5 tolerance; a zero size keeps the texel; 0.502 * 1.25 = 0.627.
	const Image grey = MakeImage(1, 1, {128, 128, 128, 128});
	const auto run = RunNode("pc.bloom", {{"surface_in", &grey}}, {{"size", 0.0}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{160, 160, 160, 160});
	CHECK(run.Output("bloom_mask").Pixels == std::vector<uint8_t>{128, 128, 128, 128});
	// Nothing passes a tolerance of 1, so the surface is unchanged.
	const auto none = RunNode("pc.bloom", {{"surface_in", &grey}}, {{"tolerance", 1.0}});
	REQUIRE(none.Ok);
	CHECK(none.Output().Pixels == grey.Pixels);
}
