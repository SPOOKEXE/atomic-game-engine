#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_pixel_sort")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	Image SortPattern() {
		return imagegraph_test::MakeImage(
			2, 2, {200, 200, 200, 255, 64, 64, 64, 255, 64, 64, 64, 255, 200, 200, 200, 255}
		);
	}

	const std::vector<uint8_t> HORIZONTAL_OR_DOWN{
		64, 64, 64, 255, 200, 200, 200, 255, 64, 64, 64, 255, 200, 200, 200, 255
	};
	const std::vector<uint8_t> HORIZONTAL_OR_UP{
		200, 200, 200, 255, 64, 64, 64, 255, 200, 200, 200, 255, 64, 64, 64, 255
	};
	const std::vector<uint8_t> ORIGINAL{
		200, 200, 200, 255, 64, 64, 64, 255, 64, 64, 64, 255, 200, 200, 200, 255
	};
}

TEST_CASE("Pixel Sort floors and wraps quarter-turn direction values", "[imagegraph][source_2d]") {
	const Image image = SortPattern();
	struct DirectionCase {
		int64_t Degrees;
		const std::vector<uint8_t> *Expected;
	};
	const std::array<DirectionCase, 10> cases{
		{{0, &HORIZONTAL_OR_DOWN},
		 {89, &HORIZONTAL_OR_DOWN},
		 {90, &ORIGINAL},
		 {180, &HORIZONTAL_OR_UP},
		 {270, &ORIGINAL},
		 {360, &HORIZONTAL_OR_DOWN},
		 {-1, &ORIGINAL},
		 {-90, &ORIGINAL},
		 {-91, &HORIZONTAL_OR_UP},
		 {-360, &HORIZONTAL_OR_DOWN}}
	};
	for (const auto &[degrees, expected] : cases) {
		const auto run = RunNode(
			"pc.pixel_sort",
			{{"surface_in", &image}},
			{{"iteration", int64_t{1}}, {"direction", degrees}, {"threshold", 0.0}}
		);
		INFO("direction degrees: " << degrees);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == *expected);
	}
}

TEST_CASE("Pixel Sort rounds integer inputs half even", "[imagegraph][source_2d]") {
	const Image image = SortPattern();
	const auto roundedHorizontal = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"direction", 89.4}, {"threshold", 0.0}}
	);
	REQUIRE(roundedHorizontal.Ok);
	CHECK(roundedHorizontal.Output().Pixels == HORIZONTAL_OR_DOWN);
	const auto roundedVertical = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"direction", 89.5}, {"threshold", 0.0}}
	);
	REQUIRE(roundedVertical.Ok);
	CHECK(roundedVertical.Output().Pixels == ORIGINAL);
	const auto rounded180 = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"direction", 179.5}, {"threshold", 0.0}}
	);
	REQUIRE(rounded180.Ok);
	CHECK(rounded180.Output().Pixels == HORIZONTAL_OR_UP);

	const Image line = imagegraph_test::MakeImage(3, 1, {240, 240, 240, 255, 96, 96, 96, 255, 8, 8, 8, 255});
	const std::vector<uint8_t> twoPasses{96, 96, 96, 255, 8, 8, 8, 255, 240, 240, 240, 255};
	for (double iterations : {1.5, 2.5}) {
		const auto rounded = RunNode(
			"pc.pixel_sort",
			{{"surface_in", &line}},
			{{"iteration", iterations}, {"direction", int64_t{0}}, {"threshold", 0.0}}
		);
		REQUIRE(rounded.Ok);
		CHECK(rounded.Output().Pixels == twoPasses);
	}
	const auto zeroIterations =
		RunNode("pc.pixel_sort", {{"surface_in", &image}}, {{"iteration", .5}, {"direction", 180.}});
	REQUIRE(zeroIterations.Ok);
	CHECK(zeroIterations.Output().Pixels == image.Pixels);
}

TEST_CASE("Pixel Sort uses strict brightness comparisons and ignores alpha", "[imagegraph][source_2d]") {
	const Image image = imagegraph_test::MakeImage(2, 1, {200, 200, 200, 0, 0, 0, 0, 255});
	const auto atThreshold =
		RunNode("pc.pixel_sort", {{"surface_in", &image}}, {{"iteration", int64_t{1}}, {"threshold", 0.0}});
	REQUIRE(atThreshold.Ok);
	CHECK(atThreshold.Output().Pixels == image.Pixels);

	const Image darker = imagegraph_test::MakeImage(2, 1, {200, 200, 200, 0, 64, 64, 64, 255});
	const auto asymmetric =
		RunNode("pc.pixel_sort", {{"surface_in", &darker}}, {{"iteration", int64_t{1}}, {"threshold", 0.0}});
	REQUIRE(asymmetric.Ok);
	CHECK(asymmetric.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255, 200, 200, 200, 0});

	const Image tied = imagegraph_test::MakeImage(2, 1, {64, 64, 64, 0, 64, 64, 64, 255});
	const auto equalBrightness =
		RunNode("pc.pixel_sort", {{"surface_in", &tied}}, {{"iteration", int64_t{1}}});
	REQUIRE(equalBrightness.Ok);
	CHECK(equalBrightness.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255, 64, 64, 64, 255});
}

TEST_CASE(
	"Pixel Sort preserves boundary pixels and skips modifiers on early identity", "[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(1, 2, {200, 0, 0, 255, 20, 0, 0, 255});
	const auto sorted = RunNode(
		"pc.pixel_sort", {{"surface_in", &image}}, {{"iteration", int64_t{1}}, {"direction", int64_t{90}}}
	);
	REQUIRE(sorted.Ok);
	CHECK(sorted.Output().Pixels == image.Pixels);

	const Image blackMask = imagegraph_test::MakeImage(1, 1, {0, 0, 0, 255});
	const auto identity = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}, {"mask", &blackMask}},
		{{"iteration", int64_t{0}}, {"mix", 0.0}, {"direction", 180}, {"channel", int64_t{0}}}
	);
	REQUIRE(identity.Ok);
	CHECK(identity.Output().Pixels == image.Pixels);

	const auto inactive = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}, {"mask", &blackMask}},
		{{"active", false}, {"iteration", int64_t{-1}}, {"mix", 0.0}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);

	const Image malformedMask{1, 1, {255}, 0};
	const auto invalidIdentity = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}, {"mask", &malformedMask}},
		{{"iteration", int64_t{0}}, {"mix", 0.0}, {"direction", 180}, {"channel", int64_t{0}}}
	);
	CHECK_FALSE(invalidIdentity.Ok);
	CHECK(invalidIdentity.Code == Status::InvalidValue);
	CHECK(invalidIdentity.Port == "mask");
	CHECK(invalidIdentity.Message == "input surface layout is invalid");

	const auto invalidInactive = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &image}, {"mask", &malformedMask}},
		{{"active", false}, {"iteration", int64_t{-1}}, {"mix", 0.0}}
	);
	CHECK_FALSE(invalidInactive.Ok);
	CHECK(invalidInactive.Code == Status::InvalidValue);
	CHECK(invalidInactive.Port == "mask");
	CHECK(invalidInactive.Message == "input surface layout is invalid");
}

TEST_CASE("Pixel Sort vertical passes start at the first interior pair", "[imagegraph][source_2d]") {
	const Image line = imagegraph_test::MakeImage(1, 3, {240, 240, 240, 255, 96, 96, 96, 255, 8, 8, 8, 255});
	const auto onePass = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &line}},
		{{"iteration", int64_t{1}}, {"direction", int64_t{90}}, {"threshold", 0.0}}
	);
	REQUIRE(onePass.Ok);
	CHECK(onePass.Output().Pixels == std::vector<uint8_t>{240, 240, 240, 255, 8, 8, 8, 255, 96, 96, 96, 255});

	const auto twoPasses = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &line}},
		{{"iteration", int64_t{2}}, {"direction", int64_t{90}}, {"threshold", 0.0}}
	);
	REQUIRE(twoPasses.Ok);
	CHECK(
		twoPasses.Output().Pixels == std::vector<uint8_t>{8, 8, 8, 255, 240, 240, 240, 255, 96, 96, 96, 255}
	);
}

TEST_CASE("Pixel Sort keeps selected surface depth and typed source samples", "[imagegraph][source_2d]") {
	Image source{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(engine::imagegraph::detail::WritePixel(source, 0, 0, {2.0, .5, .25, .25}));
	REQUIRE(engine::imagegraph::detail::WritePixel(source, 1, 0, {.25, .5, .25, .75}));
	const auto inherited = RunNode("pc.pixel_sort", {{"surface_in", &source}}, {{"iteration", int64_t{0}}});
	REQUIRE(inherited.Ok);
	CHECK(inherited.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(inherited.Output().Pixels == source.Pixels);

	const Image byteSource = imagegraph_test::MakeImage(1, 1, {255, 64, 0, 128});
	const auto selected = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &byteSource}},
		{{"iteration", int64_t{0}}, {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(selected.Ok);
	CHECK(selected.Output().Format == SurfaceFormat::RGBA32Float);
	const auto pixel = engine::imagegraph::detail::ReadPixel(selected.Output(), 0, 0);
	CHECK(pixel[0] == Catch::Approx(1.0));
	CHECK(pixel[1] == Catch::Approx(64.0 / 255.0));
	CHECK(pixel[2] == Catch::Approx(0.0));
	CHECK(pixel[3] == Catch::Approx(128.0 / 255.0));
}

TEST_CASE(
	"Pixel Sort positive iterations quantize HDR samples through RGBA8 scratch", "[imagegraph][source_2d]"
) {
	Image source{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(engine::imagegraph::detail::WritePixel(source, 0, 0, {2.0019, .5, -.25, .25}));
	REQUIRE(engine::imagegraph::detail::WritePixel(source, 1, 0, {.25, .5, .25, .75}));
	const auto sorted = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &source}},
		{{"iteration", int64_t{1}}, {"direction", int64_t{0}}, {"threshold", 0.0}}
	);
	REQUIRE(sorted.Ok);
	CHECK(sorted.Output().Format == SurfaceFormat::RGBA32Float);
	const auto first = engine::imagegraph::detail::ReadPixel(sorted.Output(), 0, 0);
	const auto second = engine::imagegraph::detail::ReadPixel(sorted.Output(), 1, 0);
	CHECK(first[0] == Catch::Approx(64.0 / 255.0));
	CHECK(first[1] == Catch::Approx(128.0 / 255.0));
	CHECK(first[2] == Catch::Approx(64.0 / 255.0));
	CHECK(first[3] == Catch::Approx(191.0 / 255.0));
	CHECK(second[0] == Catch::Approx(1.0));
	CHECK(second[1] == Catch::Approx(128.0 / 255.0));
	CHECK(second[2] == Catch::Approx(0.0));
	CHECK(second[3] == Catch::Approx(64.0 / 255.0));
}
