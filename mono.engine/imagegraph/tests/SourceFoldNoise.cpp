#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_fold_noise")
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
namespace {
	const std::array<std::pair<std::string_view, Value>, 15> &FoldDefaults() {
		static const std::array<std::pair<std::string_view, Value>, 15> defaults{
			{{"dimension", Vector2{8, 6}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{.13, -.19}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{.7, .4}},
			 {"iteration", int64_t{3}},
			 {"stretch", 1.7},
			 {"amplitude", 1.2},
			 {"detail", Vector2{2.3, .8}},
			 {"mode", EnumValue{0}},
			 {"rotation", 23.0},
			 {"level_in", Vector2{.1, 1.2}},
			 {"level_out", Vector2{-.2, 1.3}},
			 {"uv_mix", 1.0},
			 {"attribute_color_depth", EnumValue{5}}}
		};
		return defaults;
	}
	Image FloatImage(uint32_t width, uint32_t height, const std::vector<SurfacePixel> &pixels) {
		Image image{
			width,
			height,
			std::vector<uint8_t>(static_cast<size_t>(width) * height * 16),
			0,
			SurfaceFormat::RGBA32Float
		};
		REQUIRE(pixels.size() == static_cast<size_t>(width) * height);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixels[static_cast<size_t>(y) * width + x]));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	NodeRun Fold(
		const std::vector<std::pair<std::string_view, Value>> &overrides = {},
		const std::vector<std::pair<std::string_view, const Image *>> &images = {}
	) {
		const CatalogueEntry *entry = FindCatalogueEntry("pc.fold_noise");
		REQUIRE(entry);
		const detail::Executor executor = detail::FindExecutor("pc.fold_noise");
		REQUIRE(executor);
		Node node{"fold", "pc.fold_noise", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			const auto given = std::find_if(overrides.begin(), overrides.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != overrides.end()) {
				std::erase_if(context.CatalogueDefaultInputs, [&](std::string_view id) {
					return id == input.Id;
				});
				context.Values.emplace_back(input.Id, given->second);
				continue;
			}
			const auto &defaults = FoldDefaults();
			const auto sourceDefault = std::find_if(defaults.begin(), defaults.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (sourceDefault != defaults.end()) {
				context.Values.emplace_back(input.Id, sourceDefault->second);
				continue;
			}
			if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		NodeRun run;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	void CheckPixel(const NodeRun &run, SurfacePixel expected, uint32_t x = 0, uint32_t y = 0) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const SurfacePixel actual = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(1e-5));
	}
	Image UVMap() {
		return FloatImage(2, 2, {{.12, .9, 0, .2}, {.91, .65, 0, .4}, {.4, .15, 0, .6}, {.65, .3, 0, .8}});
	}
}

TEST_CASE("Fold Noise matches independent binary32 source references", "[source_fold_noise]") {
	struct Reference {
		std::string_view name;
		std::vector<std::pair<std::string_view, Value>> overrides;
		uint32_t x;
		uint32_t y;
		SurfacePixel pixel;
		bool uvMap = false;
	};
	const std::array references{
		Reference{"baseline", {}, 3, 2, {.6838211417, .6838211417, .6838211417, 1.7481355667}},
		Reference{"top-left", {}, 0, 0, {.3982968628, .3982968628, .3982968628, 1.5387511253}},
		Reference{"bottom-right", {}, 7, 5, {.8858677149, .8858677149, .8858677149, 1.8963030577}},
		Reference{"center", {}, 4, 3, {.8268257976, .8268257976, .8268257976, 1.8530056477}},
		Reference{"interior-low", {}, 1, 4, {.6547044516, .6547044516, .6547044516, 1.7267832756}},
		Reference{"interior", {}, 2, 1, {.5669130683, .5669130683, .5669130683, 1.6624029875}},
		Reference{"map-mode", {{"mode", EnumValue{1}}}, 3, 2, {-.1740373373, .6708240509, -.3363636434, 1}},
		Reference{
			"zero-iteration",
			{{"iteration", int64_t{0}}},
			3,
			2,
			{1.2355443239, 1.2355443239, 1.2355443239, 2.1527326107}
		},
		Reference{
			"negative-iteration",
			{{"iteration", int64_t{-3}}},
			3,
			2,
			{1.2355443239, 1.2355443239, 1.2355443239, 2.1527326107}
		},
		Reference{
			"one-iteration",
			{{"iteration", int64_t{1}}},
			3,
			2,
			{.7108889818, .7108889818, .7108889818, 1.7679853439}
		},
		Reference{
			"six-iterations",
			{{"iteration", int64_t{6}}},
			3,
			2,
			{.0429074168, .0429074168, .0429074168, 1.2781320810}
		},
		Reference{
			"iteration-half-even-down",
			{{"iteration", 2.5}},
			3,
			2,
			{.6669075489, .6669075489, .6669075489, 1.73573231697}
		},
		Reference{
			"iteration-half-even-up",
			{{"iteration", 3.5}},
			3,
			2,
			{1.0819145441, 1.0819145441, 1.0819145441, 2.0400707722}
		},
		Reference{
			"zero-stretch", {{"stretch", 0.0}}, 3, 2, {.5176774859, .5176774859, .5176774859, 1.6262968779}
		},
		Reference{
			"negative-stretch",
			{{"stretch", -1.7}},
			3,
			2,
			{.8204978704, .8204978704, .8204978704, 1.8483651876}
		},
		Reference{
			"zero-amplitude",
			{{"amplitude", 0.0}},
			3,
			2,
			{1.5921093225, 1.5921093225, 1.5921093225, 2.4142136574}
		},
		Reference{
			"negative-amplitude",
			{{"amplitude", -1.2}},
			3,
			2,
			{.5037001967, .5037001967, .5037001967, 1.6160469055}
		},
		Reference{
			"zero-detail",
			{{"detail", Vector2{0, 0}}},
			3,
			2,
			{1.3843454123, 1.3843454123, 1.3843454123, 2.2618532181}
		},
		Reference{
			"negative-detail",
			{{"detail", Vector2{-2.3, .8}}},
			3,
			2,
			{1.3139748573, 1.3139748573, 1.3139748573, 2.2102482319}
		},
		Reference{
			"zero-rotation", {{"rotation", 0.0}}, 3, 2, {.6377486587, .6377486587, .6377486587, 1.7143490314}
		},
		Reference{
			"negative-rotation",
			{{"rotation", -71.0}},
			3,
			2,
			{.8321820498, .8321820498, .8321820498, 1.8569335938}
		},
		Reference{
			"reference-position",
			{{"position_unit", EnumValue{1}}},
			3,
			2,
			{.7169004083, .7169004083, .7169004083, 1.7723937035}
		},
		Reference{
			"negative-scale",
			{{"scale", Vector2{-.7, .4}}},
			3,
			2,
			{.4843164682, .4843164682, .4843164682, 1.6018321514}
		},
		Reference{
			"zero-scale",
			{{"scale", Vector2{0, 0}}},
			3,
			2,
			{.3696500063, .3696500063, .3696500063, 1.5177433491}
		},
		Reference{
			"reversed-level-in",
			{{"level_in", Vector2{1.2, .1}}},
			3,
			2,
			{.4161787927, .4161787927, .4161787927, 1.7481355667}
		},
		Reference{
			"reversed-level-out",
			{{"level_out", Vector2{1.3, -.2}}},
			3,
			2,
			{.4161787927, .4161787927, .4161787927, 1.7481355667}
		},
		Reference{
			"level-identity",
			{{"level_in", Vector2{0, 1}}, {"level_out", Vector2{0, 1}}},
			3,
			2,
			{.7481355667, .7481355667, .7481355667, 1.7481355667}
		},
		Reference{
			"uv-map-quad", {{"uv_mix", .65}}, 3, 2, {.5176727772, .5176727772, .5176727772, .3252587020}, true
		},
		Reference{
			"uv-alpha-at-zero-mix",
			{{"uv_mix", 0.0}},
			3,
			2,
			{.6838211417, .6838211417, .6838211417, .3496271074},
			true
		},
		Reference{
			"uv-map-extrapolation",
			{{"uv_mix", 1.3}},
			3,
			2,
			{.3713271320, .3713271320, .3713271320, .3037946522},
			true
		},
		Reference{
			"fractional-covered",
			{{"dimension", Vector2{3.6, 2.6}}},
			3,
			2,
			{.8830830455, .8830830455, .8830830455, 1.8942608833}
		},
		Reference{"fractional-uncovered", {{"dimension", Vector2{2.5, 1.5}}}, 0, 1, {0, 0, 0, 0}},
		Reference{
			"odd-canvas",
			{{"dimension", Vector2{3, 3}}},
			2,
			2,
			{1.0920770168, 1.0920770168, 1.0920770168, 2.0475232601}
		},
		Reference{
			"map-mode-zero-iteration",
			{{"mode", EnumValue{1}}, {"iteration", int64_t{0}}},
			3,
			2,
			{.7854996324, .7646894455, -.3363636434, 1}
		}
	};
	const Image uv = UVMap();
	for (const auto &reference : references) {
		CAPTURE(reference.name);
		std::vector<std::pair<std::string_view, const Image *>> images;
		if (reference.uvMap) images.emplace_back("uv_map", &uv);
		const auto run = Fold(reference.overrides, images);
		CheckPixel(run, reference.pixel, reference.x, reference.y);
	}
}

TEST_CASE("Fold Noise skips loop-only inputs for nonpositive iteration", "[source_fold_noise]") {
	const auto zero = Fold({{"iteration", int64_t{0}}});
	const auto unused = Fold(
		{{"iteration", int64_t{-2}},
		 {"stretch", std::numeric_limits<double>::quiet_NaN()},
		 {"amplitude", std::numeric_limits<double>::infinity()},
		 {"detail", Vector2{std::numeric_limits<double>::quiet_NaN(), 1}}}
	);
	REQUIRE(zero.Ok);
	INFO(unused.Port << ": " << unused.Message);
	REQUIRE(unused.Ok);
	CHECK(zero.Output().Pixels == unused.Output().Pixels);
}

TEST_CASE("Fold Noise iteration rounds half even and bounds its source integer", "[source_fold_noise]") {
	const auto two = Fold({{"iteration", int64_t{2}}});
	const auto twoPointFive = Fold({{"iteration", 2.5}});
	const auto four = Fold({{"iteration", int64_t{4}}});
	const auto threePointFive = Fold({{"iteration", 3.5}});
	REQUIRE(two.Ok);
	REQUIRE(twoPointFive.Ok);
	REQUIRE(four.Ok);
	REQUIRE(threePointFive.Ok);
	CHECK(two.Output().Pixels == twoPointFive.Output().Pixels);
	CHECK(four.Output().Pixels == threePointFive.Output().Pixels);
	for (double iteration : {2147483648.0, -2147483649.0, std::numeric_limits<double>::infinity()}) {
		CAPTURE(iteration);
		const auto run = Fold({{"iteration", iteration}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "iteration");
	}
}

TEST_CASE("Fold Noise refuses equal level input endpoints and active raw samplers", "[source_fold_noise]") {
	const auto equal = Fold({{"level_in", Vector2{.5, .5}}});
	CHECK_FALSE(equal.Ok);
	CHECK(equal.Code == Status::UnsupportedExecution);
	CHECK(equal.Port == "level_in");
	for (const std::string_view port : {"uv_map", "mask"}) {
		const auto run = Fold({{port, AtlasValue{}}});
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
	}
}

TEST_CASE("Fold Noise preserves all seven output depths and shared mask behavior", "[source_fold_noise]") {
	const Image mask{1, 1, {128, 128, 128, 255}, 0};
	const SurfacePixel expectedPixels[]{
		{.6666666865, .6666666865, .6666666865, .5333333611},
		{.6823529601, .6823529601, .6823529601, .5019608140},
		{.68212890625, .68212890625, .68212890625, .87841796875},
		{.6823529601, .6823529601, .6823529601, .8784313798},
		{.6823529601, 0, 0, 1},
		{.68212890625, 0, 0, 1},
		{.6823529601, 0, 0, 1}
	};
	for (int64_t depth = 2; depth <= 8; ++depth) {
		CAPTURE(depth);
		const auto run = Fold({{"attribute_color_depth", EnumValue{depth}}}, {{"mask", &mask}});
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto expectedFormat = SourceSurfaceFormat(depth);
		REQUIRE(expectedFormat.has_value());
		CHECK(run.Output().Format == *expectedFormat);
		CHECK(run.Output().Width == 8);
		CHECK(run.Output().Height == 6);
		CheckPixel(run, expectedPixels[static_cast<size_t>(depth - 2)], 3, 2);
	}
	const auto alphaOnly =
		Fold({{"attribute_color_depth", EnumValue{5}}, {"mask_alpha_only", true}}, {{"mask", &mask}});
	const auto defaultMask = Fold({{"attribute_color_depth", EnumValue{5}}}, {{"mask", &mask}});
	REQUIRE(alphaOnly.Ok);
	REQUIRE(defaultMask.Ok);
	CHECK(alphaOnly.Output().Pixels == defaultMask.Output().Pixels);
}

TEST_CASE("Fold Noise reports consumed invalid values and half storage overflow", "[source_fold_noise]") {
	const std::array<std::pair<std::string_view, Value>, 7> invalid{
		{{"stretch", std::numeric_limits<double>::quiet_NaN()},
		 {"amplitude", std::numeric_limits<double>::infinity()},
		 {"detail", Vector2{std::numeric_limits<double>::infinity(), 1}},
		 {"rotation", std::numeric_limits<double>::quiet_NaN()},
		 {"scale", Vector2{std::numeric_limits<double>::infinity(), 1}},
		 {"position", Vector2{std::numeric_limits<double>::quiet_NaN(), 0}},
		 {"level_out", Vector2{0, std::numeric_limits<double>::infinity()}}}
	};
	for (const auto &[port, value] : invalid) {
		CAPTURE(port);
		const auto run =
			Fold({{"dimension", Vector2{8, 6}}, {"dimension_unit", EnumValue{0}}, {port, value}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == (port == "position" ? Status::UnsupportedExecution : Status::InvalidValue));
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
	const auto halfOverflow = Fold(
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{4}},
		 {"level_out", Vector2{1e6, 1e6}}}
	);
	CHECK_FALSE(halfOverflow.Ok);
	CHECK(halfOverflow.Code == Status::InvalidValue);
	CHECK(halfOverflow.Port == "surface_out");
	CHECK(halfOverflow.Images.empty());
}

TEST_CASE("Fold Noise clears uncovered raw sprites without consuming shader inputs", "[source_fold_noise]") {
	const auto clear = Fold(
		{{"dimension", Vector2{.2, .2}},
		 {"dimension_unit", EnumValue{0}},
		 {"scale", Vector2{std::numeric_limits<double>::quiet_NaN(), 0}},
		 {"level_in", Vector2{.5, .5}},
		 {"iteration", int64_t{3}}}
	);
	INFO(clear.Port << ": " << clear.Message);
	REQUIRE(clear.Ok);
	CHECK(clear.Output().Width == 1);
	CHECK(clear.Output().Height == 1);
	CheckPixel(clear, {0, 0, 0, 0});
}
