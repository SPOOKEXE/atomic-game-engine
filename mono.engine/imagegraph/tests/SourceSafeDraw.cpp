#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_safe_draw")
using namespace engine::imagegraph;

TEST_CASE(
	"Pixel Builder safe draws replicate single-channel red with opaque alpha", "[imagegraph][source_2d]"
) {
	for (const auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		const auto description = DescribeSurfaceFormat(format);
		REQUIRE(description);
		Image surface{1, 1, std::vector<uint8_t>(description->BytesPerPixel), 0, format};
		REQUIRE(StoreSurfacePixel(surface, 0, 0, {.25, 0, 0, 1}));
		PixelBoxValue box;
		box.Data.emplace().FixedBounds = std::array<double, 4>{0, 0, 1, 1};
		const auto cropped =
			imagegraph_test::RunNode("pc.pb_crop_pbbox", {{"surface", &surface}}, {{"pbbox", box}});
		REQUIRE(cropped.Ok);
		CHECK(cropped.Output("surface").Pixels == std::vector<uint8_t>{64, 64, 64, 255});
		const auto mirrored = imagegraph_test::RunNode(
			"pc.pb_filter_mirror", {{"surface", &surface}}, {{"pbbox", box}, {"axis", int64_t{0}}}
		);
		REQUIRE(mirrored.Ok);
		CHECK(mirrored.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	}
}

TEST_CASE("Source safe draw shader replacement survives processor staging", "[imagegraph][source_2d]") {
	for (const auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		const auto description = DescribeSurfaceFormat(format);
		REQUIRE(description);
		Image surface{1, 1, std::vector<uint8_t>(description->BytesPerPixel), 0, format};
		REQUIRE(StoreSurfacePixel(surface, 0, 0, {.25, 0, 0, 1}));
		const auto stray = imagegraph_test::RunNode(
			"pc.de_stray",
			{{"surface_in", &surface}},
			{{"attribute_color_depth", EnumValue{3}}, {"iteration", int64_t{0}}}
		);
		REQUIRE(stray.Ok);
		CHECK(stray.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
		const auto selected = imagegraph_test::RunNode(
			"pc.color_select", {{"surface_in", &surface}}, {{"attribute_color_depth", EnumValue{3}}}
		);
		REQUIRE(selected.Ok);
		CHECK(selected.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	}
}
