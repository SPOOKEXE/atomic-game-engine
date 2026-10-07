#include "../src/AtlasPayload.hpp"
#include "ComplexGeneratorFixture.hpp"
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

TEST_SUITE_ID("engine.imagegraph.source_wavelet_noise")
using namespace complex_generator_test;
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
namespace {
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	NodeRun Wavelet(
		const std::vector<std::pair<std::string_view, Value>> &overrides = {},
		const std::vector<std::pair<std::string_view, const Image *>> &images = {},
		bool includeSeed = true,
		const std::vector<std::string_view> &catalogueDefaults = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"seed", 17.25},
			{"position", Vector2{.13, -.19}},
			{"position_unit", EnumValue{1}},
			{"rotation", 23.0},
			{"scale", Vector2{4, 7}},
			{"scale_mapped", true},
			{"scale_map_range", Vector2{4, 7}},
			{"progress", .37},
			{"progress_mapped", true},
			{"progress_map_range", Vector2{.37, .37}},
			{"detail", 1.24},
			{"detail_mapped", true},
			{"detail_map_range", Vector2{1.24, 1.24}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : overrides) {
			auto found = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (found != values.end())
				found->second = value;
			else
				values.emplace_back(port, value);
		}
		const auto *entry = FindCatalogueEntry("pc.wavelet_noise");
		REQUIRE(entry);
		for (const std::string_view port : catalogueDefaults) {
			const auto *input = FindCatalogueInput(*entry, port);
			REQUIRE(input);
			const auto fallback = CatalogueDefault(*input);
			REQUIRE(fallback.has_value());
			auto found = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			REQUIRE(found != values.end());
			found->second = *fallback;
		}
		for (const auto &[control, range] : std::array<std::pair<std::string_view, std::string_view>, 3>{
				 {{"scale", "scale_map_range"},
				  {"progress", "progress_map_range"},
				  {"detail", "detail_map_range"}}
			 }) {
			const bool controlOverridden =
				std::any_of(overrides.begin(), overrides.end(), [&](const auto &item) {
					return item.first == control;
				});
			const bool rangeOverridden =
				std::any_of(overrides.begin(), overrides.end(), [&](const auto &item) {
					return item.first == range;
				});
			if (!controlOverridden || rangeOverridden) continue;
			const auto source = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == control;
			});
			const auto target = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == range;
			});
			if (const auto *pair = std::get_if<Vector2>(&source->second))
				target->second = *pair;
			else if (const auto *scalar = std::get_if<double>(&source->second))
				target->second = Vector2{*scalar, *scalar};
		}
		const auto executor = detail::FindExecutor("pc.wavelet_noise");
		REQUIRE(executor);
		Node node{"node", "pc.wavelet_noise", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			if (input.Id == "seed" && !includeSeed) continue;
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
			else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
			if (std::find(catalogueDefaults.begin(), catalogueDefaults.end(), input.Id) !=
				catalogueDefaults.end())
				context.CatalogueDefaultInputs.emplace_back(input.Id);
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
		const auto actual = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(1e-5));
	}
}

TEST_CASE("Wavelet Noise agrees with independent binary32 source references", "[source_wavelet_noise]") {
	struct Reference {
		std::string_view name;
		std::vector<std::pair<std::string_view, Value>> overrides;
		uint32_t x;
		uint32_t y;
		double gray;
		double alpha;
	};
	// grug keeps literals from separate binary32 math, not engine output.
	const std::array references{
		Reference{"baseline", {}, 3, 2, .4828312695, 1},
		Reference{"top-left", {}, 0, 0, .5630701184, 1},
		Reference{"bottom-right", {}, 7, 5, .3948994577, 1},
		Reference{"center", {}, 4, 3, .6834731698, 1},
		Reference{"negative-seed", {{"seed", -17.25}}, 3, 2, .4947664142, 1},
		Reference{"zero-seed", {{"seed", 0.0}}, 3, 2, .5069702864, 1},
		Reference{"negative-progress", {{"progress", -5.7}}, 3, 2, .5039850473, 1},
		Reference{"zero-progress", {{"progress", 0.0}}, 3, 2, .4686327577, 1},
		Reference{"large-progress", {{"progress", 137.5}}, 3, 2, .4919506013, 1},
		Reference{"detail-one", {{"detail", 1.0}}, 3, 2, .4696092904, 1},
		Reference{"detail-negative", {{"detail", -2.0}}, 3, 2, .4509594440, 1},
		Reference{"detail-small", {{"detail", .4}}, 3, 2, .4894820452, 1},
		Reference{"detail-large", {{"detail", 3.2}}, 3, 2, .5440410972, 1},
		Reference{"zero-scale", {{"scale", Vector2{0, 0}}}, 3, 2, .4799794555, 1},
		Reference{"negative-scale", {{"scale", Vector2{-4, 7}}}, 3, 2, .4082096219, 1},
		Reference{"anisotropic-scale", {{"scale", Vector2{.5, 11}}}, 3, 2, .5852336884, 1},
		Reference{"zero-rotation", {{"rotation", 0.0}}, 3, 2, .5287655592, 1},
		Reference{"negative-rotation", {{"rotation", -71.0}}, 3, 2, .4548819065, 1},
		Reference{"zero-position", {{"position", Vector2{0, 0}}}, 3, 2, .4494748414, 1},
		Reference{"pixel-position", {{"position_unit", EnumValue{0}}}, 3, 2, .4120085537, 1},
		Reference{"outside-position", {{"position", Vector2{-1.3, 2.7}}}, 3, 2, .5008944869, 1},
		Reference{"reversed-level-in", {{"level_in", Vector2{1, 0}}}, 3, 2, .5171687603, 1},
		Reference{"reversed-level-out", {{"level_out", Vector2{1.2, -.1}}}, 3, 2, .5723194480, 1},
		Reference{"offset-level-out", {{"level_out", Vector2{-.2, 1.3}}}, 3, 2, .5242468715, 1},
		Reference{"other-interior-pixel", {}, 1, 4, .5591662526, 1},
		Reference{"fractional-center", {{"dimension", Vector2{3.6, 2.6}}}, 1, 1, .5247037411, 1},
		Reference{"fractional-covered-edge", {{"dimension", Vector2{3.6, 2.6}}}, 3, 2, .3096075654, 1},
		Reference{"fractional-uncovered", {{"dimension", Vector2{2.5, 1.5}}}, 0, 1, 0, 0},
		Reference{
			"constructor-controls",
			{{"position", Vector2{0, 0}},
			 {"rotation", 0.0},
			 {"scale", Vector2{4, 4}},
			 {"progress", 0.0},
			 {"detail", 1.24}},
			3,
			2,
			.6195596457,
			1
		},
		Reference{"unused-final-detail-update", {{"scale", Vector2{0, 0}}, {"detail", 1.0e10}}, 3, 2, .5, 1}
	};
	for (const auto &reference : references) {
		CAPTURE(reference.name);
		const auto run = Wavelet(reference.overrides);
		CheckPixel(
			run, {reference.gray, reference.gray, reference.gray, reference.alpha}, reference.x, reference.y
		);
	}
}

TEST_CASE("Wavelet Noise UV map alpha survives zero UV mix", "[source_wavelet_noise]") {
	const Image uv = FloatPixel({.25, .2, 0, .4});
	const auto mapped = Wavelet({{"uv_mix", 0.0}}, {{"uv_map", &uv}});
	REQUIRE(mapped.Ok);
	CHECK(Pixel(mapped.Output(), 3, 2)[3] == Catch::Approx(.4).margin(1e-6));
	const auto plain = Wavelet({{"uv_mix", 0.0}});
	REQUIRE(plain.Ok);
	CHECK(Pixel(plain.Output(), 3, 2)[3] == 1.0);
}

TEST_CASE("Wavelet Noise control maps use mean RGB and preserve UV alpha", "[source_wavelet_noise]") {
	const Image map = FloatPixel({1, .5, 0, .1});
	const auto mapped = Wavelet(
		{{"scale_map_range", Vector2{2, 8}},
		 {"progress_map_range", Vector2{-1, 2}},
		 {"detail_map_range", Vector2{.5, 1.5}}},
		{{"scale_map", &map}, {"progress_map", &map}, {"detail_map", &map}},
		true,
		{"scale", "progress", "detail"}
	);
	const auto direct = Wavelet({{"scale", Vector2{5, 5}}, {"progress", .5}, {"detail", 1.0}});
	INFO(mapped.Port << ": " << mapped.Message << "; " << direct.Port << ": " << direct.Message);
	REQUIRE(mapped.Ok);
	REQUIRE(direct.Ok);
	CHECK(mapped.Output().Pixels == direct.Output().Pixels);

	const Image uv = FloatPixel({.5, .2, 0, .4});
	const auto withUv = Wavelet({}, {{"uv_map", &uv}}, true);
	REQUIRE(withUv.Ok);
	CHECK(Pixel(withUv.Output(), 3, 2)[3] == Catch::Approx(.4).margin(1e-6));
}

TEST_CASE("Wavelet Noise requires mapped flags and a finite active seed", "[source_wavelet_noise]") {
	for (const auto &[toggle, port] : std::array<std::pair<std::string_view, std::string_view>, 3>{
			 {{"scale_mapped", "scale"}, {"progress_mapped", "progress"}, {"detail_mapped", "detail"}}
		 }) {
		const auto run = Wavelet({{toggle, false}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
	}
	const auto missing = Wavelet({}, {}, false);
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Code == Status::UnsupportedExecution);
	CHECK(missing.Port == "seed");
	const auto nonfinite = Wavelet({{"seed", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Code == Status::InvalidValue);
	CHECK(nonfinite.Port == "seed");
}

TEST_CASE("Wavelet Noise refuses raw Atlas sampler inputs", "[source_wavelet_noise]") {
	for (const std::string_view port : {"scale_map", "progress_map", "detail_map", "uv_map", "mask"}) {
		const auto run = imagegraph_test::RunNode(
			"pc.wavelet_noise",
			{},
			{{"seed", 17.25},
			 {"scale_mapped", true},
			 {"progress_mapped", true},
			 {"detail_mapped", true},
			 {port, AtlasValue{}}}
		);
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
	}
}

TEST_CASE("Wavelet Noise rejects mask alpha overflow before publishing output", "[source_wavelet_noise]") {
	const Image uv = FloatPixel({.5, .5, 0, 3.0e38});
	const Image mask = FloatPixel({10, 10, 10, 1});
	const auto run = Wavelet({}, {{"uv_map", &uv}, {"mask", &mask}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::InvalidValue);
	CHECK(run.Port == "mask");
	CHECK(run.Images.empty());
}

TEST_CASE("Wavelet Noise refuses undefined active detail and equal levels", "[source_wavelet_noise]") {
	for (const double detail : {0.0, -1.0}) {
		const auto run = Wavelet({{"detail", detail}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "detail");
	}
	const auto equal = Wavelet({{"level_in", Vector2{.5, .5}}});
	CHECK_FALSE(equal.Ok);
	CHECK(equal.Code == Status::UnsupportedExecution);
	CHECK(equal.Port == "level_in");
}

TEST_CASE("Wavelet Noise retains requested typed output formats", "[source_wavelet_noise]") {
	constexpr std::array depths{2, 3, 4, 5, 6, 7, 8};
	for (const int64_t depth : depths) {
		const auto run = Wavelet({{"attribute_color_depth", EnumValue{depth}}});
		INFO("depth " << depth << ": " << run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *SourceSurfaceFormat(depth));
	}
}

TEST_CASE("Wavelet Noise rejects consumed float and half-storage overflow", "[source_wavelet_noise]") {
	const auto floatOverflow = Wavelet({{"scale", Vector2{0, 0}}, {"detail", 1.0e38}});
	CHECK_FALSE(floatOverflow.Ok);
	CHECK(floatOverflow.Code == Status::InvalidValue);
	CHECK(floatOverflow.Port == "detail");
	CHECK(floatOverflow.Images.empty());

	const auto halfOverflow =
		Wavelet({{"level_out", Vector2{1.0e6, 1.0e6}}, {"attribute_color_depth", EnumValue{4}}});
	CHECK_FALSE(halfOverflow.Ok);
	CHECK(halfOverflow.Code == Status::InvalidValue);
	CHECK(halfOverflow.Port == "surface_out");
	CHECK(halfOverflow.Images.empty());
}

TEST_CASE(
	"Wavelet Noise accepts an uncovered canvas without consuming shader inputs", "[source_wavelet_noise]"
) {
	const auto run = Wavelet(
		{{"dimension", Vector2{.5, .5}},
		 {"detail", 0.0},
		 {"scale_mapped", false},
		 {"progress_mapped", false},
		 {"detail_mapped", false}},
		{},
		false
	);
	INFO(run.Port << ": " << run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 1);
	CHECK(run.Output().Height == 1);
	const SurfacePixel clear{0, 0, 0, 0};
	CHECK(Pixel(run.Output()) == clear);
}
