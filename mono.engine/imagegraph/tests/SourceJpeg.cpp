#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_jpeg")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image FloatImage(uint32_t width, uint32_t height, std::vector<SurfacePixel> pixels) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 16), 0};
		image.Format = SurfaceFormat::RGBA32Float;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixels[size_t(y) * width + x]));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	imagegraph_test::NodeRun
	Jpeg(const Image &source, std::initializer_list<std::pair<std::string_view, Value>> values = {}) {
		return RunNode("pc.jpeg", {{"surface_in", &source}}, values);
	}
	Image Sample2x2() {
		return FloatImage(
			2, 2, {{.25, .5, .75, .2}, {.5, .25, .1, .4}, {.75, .1, .5, .6}, {.1, .75, .25, .8}}
		);
	}
}

TEST_CASE("JPEG deconstruct-only bypasses the DCT scratch pass", "[source_2d][jpeg]") {
	const auto source = FloatImage(1, 1, {{.25, .5, .75, .2}});
	const auto bypass = Jpeg(
		source,
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{1}},
		 {"deconstruct_only", true},
		 {"compression", 0.},
		 {"phase", 30.}}
	);
	INFO(bypass.Message);
	REQUIRE(bypass.Ok);
	const auto pixel = Pixel(bypass.Output());
	CHECK(pixel[0] == Catch::Approx(.1875).margin(.002));
	CHECK(pixel[1] == Catch::Approx(.375).margin(.002));
	CHECK(pixel[2] == Catch::Approx(.5625).margin(.002));
	CHECK(pixel[3] == Catch::Approx(1.));
}

TEST_CASE("JPEG one-pixel transform uses source PI and SQRT2 constants", "[source_2d][jpeg]") {
	const auto source = FloatImage(1, 1, {{.25, .5, .75, .2}});
	const auto run =
		Jpeg(source, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"compression", 0.}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA32Float);
	const auto pixel = Pixel(run.Output());
	CHECK(pixel[0] == Catch::Approx(.25).margin(.002));
	CHECK(pixel[1] == Catch::Approx(.5).margin(.002));
	CHECK(pixel[2] == Catch::Approx(.75).margin(.002));
	CHECK(pixel[3] == Catch::Approx(1.));
	const auto phased = Jpeg(
		source,
		{{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"compression", 0.}, {"phase", 30.}}
	);
	INFO(phased.Message);
	REQUIRE(phased.Ok);
	CHECK(Pixel(phased.Output())[0] == Catch::Approx(.140625).margin(.002));
}

TEST_CASE("JPEG transform choices zero through two produce distinct phase responses", "[source_2d][jpeg]") {
	const auto source = Sample2x2();
	const auto cosine = Jpeg(
		source,
		{{"patch_size", int64_t{2}},
		 {"reconstruction", int64_t{2}},
		 {"compression", 0.},
		 {"transformation", EnumValue{0}},
		 {"phase", 37.}}
	);
	const auto zigzag = Jpeg(
		source,
		{{"patch_size", int64_t{2}},
		 {"reconstruction", int64_t{2}},
		 {"compression", 0.},
		 {"transformation", EnumValue{1}},
		 {"phase", 37.}}
	);
	const auto smooth = Jpeg(
		source,
		{{"patch_size", int64_t{2}},
		 {"reconstruction", int64_t{2}},
		 {"compression", 0.},
		 {"transformation", EnumValue{2}},
		 {"phase", 37.}}
	);
	INFO(cosine.Message);
	INFO(zigzag.Message);
	INFO(smooth.Message);
	REQUIRE(cosine.Ok);
	REQUIRE(zigzag.Ok);
	REQUIRE(smooth.Ok);
	CHECK(Pixel(cosine.Output(), 0, 0)[0] != Catch::Approx(Pixel(zigzag.Output(), 0, 0)[0]));
	CHECK(Pixel(smooth.Output(), 1, 1)[0] != Catch::Approx(Pixel(zigzag.Output(), 1, 1)[0]));
	const auto phased = Jpeg(
		source,
		{{"patch_size", int64_t{2}},
		 {"reconstruction", int64_t{2}},
		 {"compression", 0.},
		 {"transformation", EnumValue{0}},
		 {"phase", 0.}}
	);
	INFO(phased.Message);
	REQUIRE(phased.Ok);
	CHECK(Pixel(phased.Output(), 0, 0)[0] != Catch::Approx(Pixel(cosine.Output(), 0, 0)[0]));
}

TEST_CASE(
	"JPEG Step transform is refused because the pinned shader leaves coefficients undefined",
	"[source_2d][jpeg]"
) {
	const auto source = Sample2x2();
	const auto run = Jpeg(source, {{"patch_size", int64_t{2}}, {"transformation", EnumValue{3}}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Port == "transformation");
}

TEST_CASE("JPEG clamps patch and reconstruction counts to their shader loop bounds", "[source_2d][jpeg]") {
	const auto source = Sample2x2();
	const auto patchMinimum = Jpeg(
		source, {{"patch_size", int64_t{0}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	const auto patchOne = Jpeg(
		source, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	const auto negativePatch = Jpeg(
		source, {{"patch_size", int64_t{-3}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	INFO(patchMinimum.Message);
	INFO(patchOne.Message);
	INFO(negativePatch.Message);
	REQUIRE(patchMinimum.Ok);
	REQUIRE(patchOne.Ok);
	REQUIRE(negativePatch.Ok);
	CHECK(patchMinimum.Output().Pixels == patchOne.Output().Pixels);
	CHECK(negativePatch.Output().Pixels == patchOne.Output().Pixels);
	const auto reconstructZero = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{0}}, {"deconstruct_only", true}}
	);
	const auto reconstructOne = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	const auto negativeReconstruction = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{-1}}, {"deconstruct_only", true}}
	);
	INFO(reconstructZero.Message);
	INFO(reconstructOne.Message);
	REQUIRE(reconstructZero.Ok);
	REQUIRE(reconstructOne.Ok);
	REQUIRE(negativeReconstruction.Ok);
	CHECK(Pixel(reconstructZero.Output())[0] == Catch::Approx(0.));
	CHECK(Pixel(reconstructZero.Output())[3] == Catch::Approx(1.));
	CHECK(Pixel(reconstructOne.Output())[0] != Catch::Approx(0.));
	CHECK(negativeReconstruction.Output().Pixels == reconstructZero.Output().Pixels);
}

TEST_CASE("JPEG reconstruction reads every coefficient in the requested patch", "[source_2d][jpeg]") {
	const auto source = Sample2x2();
	const auto first = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	const auto full = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{2}}, {"deconstruct_only", true}}
	);
	INFO(first.Message);
	INFO(full.Message);
	REQUIRE(first.Ok);
	REQUIRE(full.Ok);
	CHECK(Pixel(first.Output(), 0, 0)[0] != Catch::Approx(Pixel(full.Output(), 0, 0)[0]));
	CHECK(Pixel(first.Output(), 1, 1)[0] != Catch::Approx(Pixel(full.Output(), 1, 1)[0]));
}

TEST_CASE("JPEG reconstruction above patch size reads adjacent clamped samples", "[source_2d][jpeg]") {
	const auto source = FloatImage(2, 2, {{1, 0, 0, .1}, {2, 0, 0, .2}, {3, 0, 0, .3}, {4, 0, 0, .4}});
	const auto one = Jpeg(
		source,
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{1}},
		 {"transformation", EnumValue{1}},
		 {"deconstruct_only", true}}
	);
	const auto two = Jpeg(
		source,
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{2}},
		 {"transformation", EnumValue{1}},
		 {"deconstruct_only", true}}
	);
	INFO(one.Message);
	INFO(two.Message);
	REQUIRE(one.Ok);
	REQUIRE(two.Ok);
	CHECK(Pixel(one.Output(), 0, 0)[0] == Catch::Approx(1.));
	CHECK(Pixel(two.Output(), 0, 0)[0] != Catch::Approx(Pixel(one.Output(), 0, 0)[0]));
	CHECK(Pixel(two.Output(), 1, 1)[0] == Catch::Approx(.6862915).margin(.002));
}

TEST_CASE(
	"JPEG compression rounds exact half ties down, including negative coefficients", "[source_2d][jpeg]"
) {
	const auto positive = FloatImage(1, 1, {{.5, .5, .5, 1.}});
	const auto negative = FloatImage(1, 1, {{-.50000006, -.50000006, -.50000006, 1.}});
	const auto compressedPositive =
		Jpeg(positive, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"compression", 1.}});
	const auto compressedNegative =
		Jpeg(negative, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"compression", 1.}});
	INFO(compressedPositive.Message);
	INFO(compressedNegative.Message);
	REQUIRE(compressedPositive.Ok);
	REQUIRE(compressedNegative.Ok);
	CHECK(Pixel(compressedPositive.Output())[0] == Catch::Approx(0.));
	CHECK(Pixel(compressedNegative.Output())[0] == Catch::Approx(-1.));
}

TEST_CASE("JPEG reconstruct-all selects the patch-sized coefficient sweep", "[source_2d][jpeg]") {
	const auto source = Sample2x2();
	const auto bounded = Jpeg(
		source, {{"patch_size", int64_t{2}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	const auto all = Jpeg(
		source,
		{{"patch_size", int64_t{2}},
		 {"reconstruction", int64_t{1}},
		 {"deconstruct_only", true},
		 {"reconstruct_all", true}}
	);
	INFO(bounded.Message);
	INFO(all.Message);
	REQUIRE(bounded.Ok);
	REQUIRE(all.Ok);
	CHECK(Pixel(bounded.Output(), 0, 0)[0] != Catch::Approx(Pixel(all.Output(), 0, 0)[0]));
}

TEST_CASE(
	"JPEG intermediate is half float, published HDR is float32, and inactive copy is exact",
	"[source_2d][jpeg]"
) {
	const auto source = FloatImage(1, 1, {{2, .5, .25, .2}});
	const auto run = Jpeg(
		source, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"deconstruct_only", true}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA32Float);
	const auto output = Pixel(run.Output());
	CHECK(output[0] == Catch::Approx(2.).margin(.01));
	CHECK(output[1] == Catch::Approx(.5).margin(.01));
	CHECK(output[2] == Catch::Approx(.25).margin(.01));
	CHECK(output[3] == Catch::Approx(1.));

	const auto inactive = Jpeg(source, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(inactive.Output().Pixels == source.Pixels);
}
