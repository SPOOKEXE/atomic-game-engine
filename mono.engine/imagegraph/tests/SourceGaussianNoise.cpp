#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_gaussian_noise")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
	Image RawFloatPixel(std::array<float, 4> pixel) {
		Image image{1, 1, std::vector<uint8_t>(sizeof(pixel)), 0, SurfaceFormat::RGBA32Float};
		std::memcpy(image.Pixels.data(), pixel.data(), sizeof(pixel));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	void CheckGrey(const imagegraph_test::NodeRun &run, double value, double alpha = 1) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output());
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == Catch::Approx(value).margin(1e-5));
		CHECK(pixel[3] == Catch::Approx(alpha).margin(1e-5));
	}
	struct GaussianReference {
		std::string_view Name;
		uint32_t X = 3, Y = 2;
		Vector2 Dimension{8, 6}, Position{.13, -.19}, Scale{.7, .4}, LevelIn{.1, .9}, LevelOut{-.2, 1.3};
		double Seed = 17.25, Rotation = 23, Mean = .3, Varience = .8;
		bool Conversion = false;
		const Image *First = nullptr, *Second = nullptr;
		SurfacePixel Expected{};
	};
	imagegraph_test::NodeRun RunGaussian(const GaussianReference &reference) {
		imagegraph_test::NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_gaussian");
		const detail::Executor executor = detail::FindExecutor("pc.noise_gaussian");
		if (!entry || !executor) {
			run.Message = "Gaussian Noise source executor is unavailable";
			return run;
		}
		Node node{"gaussian", "pc.noise_gaussian", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		if (reference.First) context.Images.emplace_back("conv_surf_1", reference.First);
		if (reference.Second) context.Images.emplace_back("conv_surf_2", reference.Second);
		const std::array<std::pair<std::string_view, Value>, 12> values{
			{{"dimension", reference.Dimension},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", reference.Seed},
			 {"position", reference.Position},
			 {"rotation", reference.Rotation},
			 {"scale", reference.Scale},
			 {"mean", reference.Mean},
			 {"varience", reference.Varience},
			 {"level_in", reference.LevelIn},
			 {"level_out", reference.LevelOut},
			 {"use_conversion", reference.Conversion},
			 {"attribute_color_depth", EnumValue{5}}}
		};
		for (const CatalogueInput &input : entry->Inputs) {
			const auto value = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (value != values.end())
				context.Values.emplace_back(input.Id, value->second);
			else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	imagegraph_test::NodeRun Convert(const Image &first, const Image &second) {
		return RunNode(
			"pc.noise_gaussian",
			{{"conv_surf_1", &first}, {"conv_surf_2", &second}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"use_conversion", true},
			 {"mean", .5},
			 {"varience", .5},
			 {"level_in", Vector2{0, 1}},
			 {"level_out", Vector2{0, 1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
	}
}

TEST_CASE("Gaussian Noise matches independent binary32 source references", "[source_gaussian_noise]") {
	const Image conversionFirst = FloatPixel({.25, .8, .9, .1});
	const Image conversionSecond = FloatPixel({.6, .1, .2, .3});
	const Image conversionOne = FloatPixel({1, 0, 0, 0});
	const Image conversionNegativeSecond = FloatPixel({-.6, 1, 1, 1});
	const Image conversionOutsideSecond = FloatPixel({1.6, 0, 0, 0});
	const auto base = [](std::string_view name, uint32_t x, uint32_t y, SurfacePixel expected) {
		return GaussianReference{
			name,
			x,
			y,
			{8, 6},
			{.13, -.19},
			{.7, .4},
			{.1, .9},
			{-.2, 1.3},
			17.25,
			23,
			.3,
			.8,
			false,
			nullptr,
			nullptr,
			expected
		};
	};
	std::vector<GaussianReference> references{
		base("baseline", 3, 2, {-.8515656590, -.8515656590, -.8515656590, 1}),
		base("pixel-0-0", 0, 0, {.7384015918, .7384015918, .7384015918, 1}),
		base("pixel-7-5", 7, 5, {-1.2386131287, -1.2386131287, -1.2386131287, 1}),
		base("pixel-4-3", 4, 3, {.2637756467, .2637756467, .2637756467, 1}),
		base("pixel-1-4", 1, 4, {-.8042166233, -.8042166233, -.8042166233, 1}),
		base("pixel-2-1", 2, 1, {-1.0411486626, -1.0411486626, -1.0411486626, 1}),
		base("negative-seed", 3, 2, {1.3332349062, 1.3332349062, 1.3332349062, 1}),
		base("zero-seed", 3, 2, {-1.1584858894, -1.1584858894, -1.1584858894, 1}),
		base("seed-period", 3, 2, {-.8515656590, -.8515656590, -.8515656590, 1}),
		base("mean-zero", 3, 2, {-1.4140657187, -1.4140657187, -1.4140657187, 1}),
		base("mean-one", 3, 2, {.4609343410, .4609343410, .4609343410, 1}),
		base("variance-zero", 3, 2, {.1750000119, .1750000119, .1750000119, 1}),
		base("variance-negative", 3, 2, {1.2015656233, 1.2015656233, 1.2015656233, 1}),
		base("rotation-zero", 3, 2, {-.3507575989, -.3507575989, -.3507575989, 1}),
		base("rotation-negative", 3, 2, {-.8113876581, -.8113876581, -.8113876581, 1}),
		base("zero-scale", 3, 2, {-.2012828290, -.2012828290, -.2012828290, 1}),
		base("negative-scale", 3, 2, {2.8778812885, 2.8778812885, 2.8778812885, 1}),
		base("zero-position", 3, 2, {2.6017141342, 2.6017141342, 2.6017141342, 1}),
		base("reversed-levels", 3, 2, {1.9515657425, 1.9515657425, 1.9515657425, 1}),
		base("identity-levels", 3, 2, {-.2475016713, -.2475016713, -.2475016713, 1}),
		base("conversion", 3, 2, {-1.8456521034, -1.8456521034, -1.8456521034, 1}),
		base("conversion-first-one", 3, 2, {.1750000119, .1750000119, .1750000119, 1}),
		base("conversion-negative-second", 3, 2, {-1.8456521034, -1.8456521034, -1.8456521034, 1}),
		base("conversion-second-outside-unit", 3, 2, {-1.8456523418, -1.8456523418, -1.8456523418, 1}),
		base("fractional-covered", 3, 2, {1.4294736385, 1.4294736385, 1.4294736385, 1}),
		base("fractional-uncovered", 0, 1, {0, 0, 0, 0}),
		base("odd-canvas", 2, 2, {1.2452934980, 1.2452934980, 1.2452934980, 1})
	};
	references[6].Seed = -17.25;
	references[7].Seed = 0;
	references[8].Seed = 100017.25;
	references[9].Mean = 0;
	references[10].Mean = 1;
	references[11].Varience = 0;
	references[12].Varience = -.8;
	references[13].Rotation = 0;
	references[14].Rotation = -71;
	references[15].Scale = {0, 0};
	references[16].Scale = {-.7, .4};
	references[17].Position = {0, 0};
	references[18].LevelIn = {.9, .1};
	references[19].LevelIn = {0, 1};
	references[19].LevelOut = {0, 1};
	references[20].Conversion = true;
	references[20].First = &conversionFirst;
	references[20].Second = &conversionSecond;
	references[21].Conversion = true;
	references[21].First = &conversionOne;
	references[21].Second = &conversionSecond;
	references[22].Conversion = true;
	references[22].First = &conversionFirst;
	references[22].Second = &conversionNegativeSecond;
	references[23].Conversion = true;
	references[23].First = &conversionFirst;
	references[23].Second = &conversionOutsideSecond;
	references[24].Dimension = {3.6, 2.6};
	references[25].Dimension = {2.5, 1.5};
	references[26].Dimension = {3, 3};
	for (const auto &reference : references) {
		CAPTURE(reference.Name);
		const auto run = RunGaussian(reference);
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const SurfacePixel actual = Pixel(run.Output(), reference.X, reference.Y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(reference.Expected[channel]).margin(1e-5));
	}
}

TEST_CASE(
	"Gaussian Noise conversion follows Box Muller red inputs and ignores Seed", "[source_gaussian_noise]"
) {
	const Image n1 = FloatPixel({.6065306597, 0, 0, 1});
	const Image phaseZero = FloatPixel({0, 0, 0, 1});
	const Image phaseHalf = FloatPixel({.5, 0, 0, 1});
	const auto positive = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &n1}, {"conv_surf_2", &phaseZero}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"seed", std::numeric_limits<double>::quiet_NaN()},
		 {"mean", .5},
		 {"varience", .5},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CheckGrey(positive, 1.0);
	const auto negative = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &n1}, {"conv_surf_2", &phaseHalf}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"mean", .5},
		 {"varience", .5},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CheckGrey(negative, 0.0);
	const Image unit = FloatPixel({1, 0, 0, 1});
	const auto noRadius = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &unit}, {"conv_surf_2", &phaseZero}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CheckGrey(noRadius, .5);
}

TEST_CASE("Gaussian Noise conversion boolean uses the source half threshold", "[source_gaussian_noise]") {
	const auto falseFlag = RunNode(
		"pc.noise_gaussian",
		{},
		{{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"seed", 17.25}}
	);
	const auto threshold = RunNode(
		"pc.noise_gaussian",
		{},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"use_conversion", .5}}
	);
	const auto below = RunNode(
		"pc.noise_gaussian",
		{},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"use_conversion", .499}}
	);
	REQUIRE(falseFlag.Ok);
	REQUIRE(threshold.Ok);
	REQUIRE(below.Ok);
	CHECK(threshold.Output().Pixels == falseFlag.Output().Pixels);
	CHECK(below.Output().Pixels == falseFlag.Output().Pixels);
	const Image first = FloatPixel({.5, 0, 0, 1});
	const Image second = FloatPixel({.5, 0, 0, 1});
	const auto above = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &first}, {"conv_surf_2", &second}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", .500001},
		 {"mean", .5},
		 {"varience", .5}}
	);
	CHECK(above.Ok);
}

TEST_CASE("Gaussian Noise conversion validates only consumed red values", "[source_gaussian_noise]") {
	const Image zero = FloatPixel({0, 0, 0, 1});
	const Image over = FloatPixel({1.01, 0, 0, 1});
	const Image valid = FloatPixel({.5, 0, 0, 1});
	for (const Image *first : {&zero, &over}) {
		const auto run = Convert(*first, valid);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "conv_surf_1");
	}
	const Image nanSecond = RawFloatPixel({std::numeric_limits<float>::quiet_NaN(), 0, 0, 1});
	const auto nonfinite = Convert(valid, nanSecond);
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Code == Status::InvalidValue);
	CHECK(nonfinite.Port == "conv_surf_2");
	const Image largeFinite = FloatPixel({3e38, 0, 0, 1});
	const auto phaseOverflow = Convert(valid, largeFinite);
	CHECK_FALSE(phaseOverflow.Ok);
	CHECK(phaseOverflow.Code == Status::InvalidValue);
	CHECK(phaseOverflow.Port == "conv_surf_2");
	const auto equalLevels = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &valid}, {"conv_surf_2", &valid}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"level_in", Vector2{.5, .5}}}
	);
	CHECK_FALSE(equalLevels.Ok);
	CHECK(equalLevels.Code == Status::UnsupportedExecution);
	CHECK(equalLevels.Port == "level_in");
	const auto missingFirst = RunNode(
		"pc.noise_gaussian",
		{},
		{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"use_conversion", true}}
	);
	CHECK_FALSE(missingFirst.Ok);
	CHECK(missingFirst.Code == Status::UnsupportedExecution);
	CHECK(missingFirst.Port == "conv_surf_1");
	const Image validSurface = FloatPixel({.5, 0, 0, 1});
	const auto firstAtlas = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_2", &validSurface}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"conv_surf_1", AtlasValue{}}}
	);
	const auto secondAtlas = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &validSurface}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"conv_surf_2", AtlasValue{}}}
	);
	CHECK_FALSE(firstAtlas.Ok);
	CHECK(firstAtlas.Code == Status::UnsupportedExecution);
	CHECK(firstAtlas.Port == "conv_surf_1");
	CHECK_FALSE(secondAtlas.Ok);
	CHECK(secondAtlas.Code == Status::UnsupportedExecution);
	CHECK(secondAtlas.Port == "conv_surf_2");
}

TEST_CASE("Gaussian Noise retains the seven selected output formats", "[source_gaussian_noise]") {
	const Image first = FloatPixel({1, 0, 0, 1});
	const Image second = FloatPixel({0, 0, 0, 1});
	const std::array<double, 7> expected{.5333333333, .5019607843, .5, .5, .5019607843, .5000076295, .5};
	for (int64_t depth = 2; depth <= 8; ++depth) {
		CAPTURE(depth);
		const auto run = RunNode(
			"pc.noise_gaussian",
			{{"conv_surf_1", &first}, {"conv_surf_2", &second}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"use_conversion", true},
			 {"mean", .5},
			 {"varience", 0.0},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		REQUIRE(run.Ok);
		const auto format = SourceSurfaceFormat(depth);
		REQUIRE(format.has_value());
		CHECK(run.Output().Format == *format);
		const auto pixel = Pixel(run.Output());
		CHECK(pixel[0] == Catch::Approx(expected[static_cast<size_t>(depth - 2)]).margin(.035));
		CHECK(pixel[3] == Catch::Approx(1.0));
	}
}
