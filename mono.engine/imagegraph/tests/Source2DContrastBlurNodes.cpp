#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_contrast_blur")
using namespace engine::imagegraph;
TEST_CASE("Contrast Blur uses source circular weights and exact alpha gating", "[imagegraph][source_2d]") {
	const Image source{2, 1, {64, 64, 64, 255, 192, 192, 192, 255}, 0};
	const auto run = imagegraph_test::RunNode(
		"pc.blur_contrast",
		{{"surface_in", &source}},
		{{"size", 1.5}, {"threshold", 1.}, {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{85, 85, 85, 255, 171, 171, 171, 255});
	const auto strict = imagegraph_test::RunNode(
		"pc.blur_contrast",
		{{"surface_in", &source}},
		{{"size", 1.5}, {"threshold", 0.}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(strict.Ok);
	CHECK(strict.Output().Pixels == source.Pixels);
	Image alpha = source;
	alpha.Pixels[3] = 128;
	const auto gated = imagegraph_test::RunNode(
		"pc.blur_contrast",
		{{"surface_in", &alpha}},
		{{"size", 1.5}, {"threshold", 1.}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(gated.Ok);
	CHECK(gated.Output().Pixels == alpha.Pixels);
}
TEST_CASE(
	"Contrast Blur retains the source unpowered center and final gamma "
	"transform",
	"[imagegraph][source_2d]"
) {
	const Image source{1, 1, {64, 128, 192, 255}, 0};
	const auto run = imagegraph_test::RunNode(
		"pc.blur_contrast", {{"surface_in", &source}}, {{"size", 0.}, {"gamma_correction", true}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{136, 186, 224, 255});
	const auto unchanged =
		imagegraph_test::RunNode("pc.blur_contrast", {{"surface_in", &source}}, {{"size", 0.}});
	REQUIRE(unchanged.Ok);
	CHECK(unchanged.Output().Pixels == source.Pixels);
}
TEST_CASE("Contrast Blur scalar safe draw ignores replacement shader controls", "[imagegraph][source_2d]") {
	for (const auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		const auto desc = DescribeSurfaceFormat(format);
		REQUIRE(desc);
		Image source{1, 1, std::vector<uint8_t>(desc->BytesPerPixel), 0, format};
		REQUIRE(StoreSurfacePixel(source, 0, 0, {.25, 0, 0, 1}));
		const auto run = imagegraph_test::RunNode(
			"pc.blur_contrast",
			{{"surface_in", &source}},
			{{"attribute_color_depth", EnumValue{3}}, {"size", 65.}, {"gamma_correction", true}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	}
}
TEST_CASE(
	"Contrast Blur refuses source gamma powers on negative floating-point pixels", "[imagegraph][source_2d]"
) {
	Image source{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {-1, .5, .75, 1}));
	const auto raw = imagegraph_test::RunNode("pc.blur_contrast", {{"surface_in", &source}}, {{"size", 0.}});
	REQUIRE(raw.Ok);
	CHECK(raw.Output().Pixels == source.Pixels);
	const auto gamma = imagegraph_test::RunNode(
		"pc.blur_contrast", {{"surface_in", &source}}, {{"size", 0.}, {"gamma_correction", true}}
	);
	CHECK_FALSE(gamma.Ok);
	CHECK(gamma.Code == Status::UnsupportedExecution);
	CHECK(gamma.Port == "surface_in");
}
