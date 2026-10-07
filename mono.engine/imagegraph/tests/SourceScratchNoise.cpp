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

TEST_SUITE_ID("engine.imagegraph.source_scratch_noise")
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
namespace {
	const std::array<std::pair<std::string_view, Value>, 18> &ScratchDefaults() {
		static const std::array<std::pair<std::string_view, Value>, 18> defaults{
			{{"dimension", Vector2{8, 6}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{.13, -.19}},
			 {"position_unit", EnumValue{0}},
			 {"rotation", 23.0},
			 {"scale", Vector2{.7, .4}},
			 {"seed", 17.25},
			 {"thickness", .23},
			 {"thickness_mapped", true},
			 {"wavyness", .61},
			 {"wavyness_mapped", true},
			 {"softness", .42},
			 {"softness_mapped", true},
			 {"octaves", int64_t{3}},
			 {"octave_scale", .8},
			 {"octave_shift", Vector2{.3, -.2}},
			 {"octave_rotation", 30.0},
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
	NodeRun Scratch(
		const std::vector<std::pair<std::string_view, Value>> &overrides = {},
		const std::vector<std::pair<std::string_view, const Image *>> &images = {},
		bool includeSeed = true
	) {
		const CatalogueEntry *entry = FindCatalogueEntry("pc.noise_scratch");
		REQUIRE(entry);
		const detail::Executor executor = detail::FindExecutor("pc.noise_scratch");
		REQUIRE(executor);
		Node node{"scratch", "pc.noise_scratch", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			if (input.Id == "seed" && !includeSeed) continue;
			const auto given = std::find_if(overrides.begin(), overrides.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != overrides.end()) {
				context.Values.emplace_back(input.Id, given->second);
				continue;
			}
			const auto &defaults = ScratchDefaults();
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

TEST_CASE("Scratch Noise matches independent binary32 source references", "[source_scratch_noise]") {
	struct Reference {
		std::string_view name;
		std::vector<std::pair<std::string_view, Value>> overrides;
		uint32_t x;
		uint32_t y;
		SurfacePixel pixel;
		uint8_t mapMode = 0;
	};
	const std::array references{
		Reference{"baseline", {}, 3, 2, {.2766661048, .2766661048, .2766661048, 1}},
		Reference{"top-left", {}, 0, 0, {0, 0, 0, 1}},
		Reference{"bottom-right", {}, 7, 5, {0, 0, 0, 1}},
		Reference{"center", {}, 4, 3, {0, 0, 0, 1}},
		Reference{"lower-interior", {}, 1, 4, {.3741484284, .3741484284, .3741484284, 1}},
		Reference{"interior", {}, 2, 1, {0, 0, 0, 1}},
		Reference{"interior-two", {}, 3, 1, {.2425137609, .2425137609, .2425137609, 1}},
		Reference{"negative-seed", {{"seed", -17.25}}, 3, 2, {.1244975254, .1244975254, .1244975254, 1}},
		Reference{"zero-seed", {{"seed", 0.0}}, 3, 2, {.1534489840, .1534489840, .1534489840, 1}},
		Reference{"zero-thickness", {{"thickness", 0.0}}, 3, 2, {0, 0, 0, 1}},
		Reference{"negative-thickness", {{"thickness", -.2}}, 3, 2, {0, 0, 0, 1}},
		Reference{"thin-thickness", {{"thickness", .01}}, 3, 2, {0, 0, 0, 1}},
		Reference{"zero-wavyness", {{"wavyness", 0.0}}, 3, 2, {.5853056908, .5853056908, .5853056908, 1}},
		Reference{
			"negative-wavyness", {{"wavyness", -1.2}}, 3, 2, {.7991598248, .7991598248, .7991598248, 1}
		},
		Reference{
			"negative-softness", {{"softness", -.42}}, 3, 2, {1.3697626591, 1.3697626591, 1.3697626591, 1}
		},
		Reference{"one-octave", {{"octaves", int64_t{1}}}, 3, 2, {.2766661048, .2766661048, .2766661048, 1}},
		Reference{
			"eight-octaves", {{"octaves", int64_t{8}}}, 3, 2, {.2766661048, .2766661048, .2766661048, 1}
		},
		Reference{"zero-octaves", {{"octaves", int64_t{0}}}, 3, 2, {0, 0, 0, 1}},
		Reference{"negative-octaves", {{"octaves", int64_t{-3}}}, 3, 2, {0, 0, 0, 1}},
		Reference{
			"negative-scale", {{"scale", Vector2{-.7, .4}}}, 3, 2, {.5318170786, .5318170786, .5318170786, 1}
		},
		Reference{"zero-rotation", {{"rotation", 0.0}}, 3, 2, {.0028388391, .0028388391, .0028388391, 1}},
		Reference{
			"negative-rotation", {{"rotation", -71.0}}, 3, 2, {.0311269294, .0311269294, .0311269294, 1}
		},
		Reference{
			"reference-position",
			{{"position_unit", EnumValue{1}}},
			3,
			2,
			{.8335027695, .8335027695, .8335027695, 1}
		},
		Reference{
			"zero-octave-shift",
			{{"octave_shift", Vector2{0, 0}}},
			3,
			2,
			{.2766661048, .2766661048, .2766661048, 1}
		},
		Reference{
			"negative-octave-rotation",
			{{"octave_rotation", -45.0}},
			3,
			2,
			{.2766661048, .2766661048, .2766661048, 1}
		},
		Reference{
			"negative-octave-scale", {{"octave_scale", -.8}}, 3, 2, {.6145256758, .6145256758, .6145256758, 1}
		},
		Reference{"mapped-uv-quad", {{"uv_mix", .65}}, 3, 2, {0, 0, 0, .2}, 1},
		Reference{
			"uv-alpha-at-zero-mix", {{"uv_mix", 0.0}}, 3, 2, {.2766661048, .2766661048, .2766661048, .2}, 1
		},
		Reference{
			"uv-mix-extrapolation", {{"uv_mix", 1.3}}, 3, 2, {.0087353662, .0087353662, .0087353662, .2}, 1
		},
		Reference{
			"mean-rgb-map-controls",
			{{"thickness", Vector2{.1, .36}},
			 {"wavyness", Vector2{.2, 1.02}},
			 {"softness", Vector2{.2, .64}}},
			3,
			2,
			{.2766661048, .2766661048, .2766661048, 1},
			2
		},
		Reference{
			"odd-edge-helper-quad",
			{{"dimension", Vector2{3, 3}}, {"uv_mix", .25}},
			2,
			2,
			{.0193221234, .0193221234, .0193221234, .8},
			1
		},
		Reference{
			"fractional-covered",
			{{"dimension", Vector2{3.6, 2.6}}},
			3,
			2,
			{.0600271113, .0600271113, .0600271113, 1}
		},
		Reference{"fractional-uncovered", {{"dimension", Vector2{2.5, 1.5}}}, 0, 1, {0, 0, 0, 0}},
		Reference{"late-octaves-base", {}, 5, 2, {.2438458800, .2438458800, .2438458800, 1}},
		Reference{
			"late-octaves-change",
			{{"octaves", int64_t{8}}},
			5,
			2,
			{1.3643546104, 1.3643546104, 1.3643546104, 1}
		},
		Reference{"shift-base", {}, 4, 2, {0, 0, 0, 1}},
		Reference{
			"shift-change",
			{{"octave_shift", Vector2{0, 0}}},
			4,
			2,
			{.8250283003, .8250283003, .8250283003, 1}
		},
		Reference{"octave-angle-base", {}, 5, 1, {0, 0, 0, 1}},
		Reference{
			"octave-angle-change",
			{{"octave_rotation", -45.0}},
			5,
			1,
			{1.2693743706, 1.2693743706, 1.2693743706, 1}
		},
		Reference{"width-base", {}, 3, 0, {0, 0, 0, 1}},
		Reference{
			"width-sign-change", {{"octave_scale", -.8}}, 3, 0, {1.3676226139, 1.3676226139, 1.3676226139, 1}
		}
	};
	const Image uv = UVMap();
	const Image map = FloatImage(1, 1, {{1, .5, 0, .1}});
	for (const auto &reference : references) {
		CAPTURE(reference.name);
		std::vector<std::pair<std::string_view, const Image *>> images;
		if (reference.mapMode == 1) images.emplace_back("uv_map", &uv);
		if (reference.mapMode == 2) {
			images.emplace_back("thickness_map", &map);
			images.emplace_back("wavyness_map", &map);
			images.emplace_back("softness_map", &map);
		}
		const auto run = Scratch(reference.overrides, images);
		CheckPixel(run, reference.pixel, reference.x, reference.y);
	}
}

TEST_CASE("Scratch Noise requires mapped controls and finite active inputs", "[source_scratch_noise]") {
	for (const auto &[toggle, port] : std::array<std::pair<std::string_view, std::string_view>, 3>{
			 {{"thickness_mapped", "thickness"},
			  {"wavyness_mapped", "wavyness"},
			  {"softness_mapped", "softness"}}
		 }) {
		const auto run = Scratch({{toggle, false}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
	}
	const auto missingSeed = Scratch({}, {}, false);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	const auto nonfiniteSeed = Scratch({{"seed", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfiniteSeed.Ok);
	CHECK(nonfiniteSeed.Code == Status::InvalidValue);
	CHECK(nonfiniteSeed.Port == "seed");
	const auto zeroScale = Scratch({{"scale", Vector2{0, .4}}});
	CHECK_FALSE(zeroScale.Ok);
	CHECK(zeroScale.Code == Status::UnsupportedExecution);
	CHECK(zeroScale.Port == "scale");
	const auto equalEdges = Scratch({{"softness", 0.0}});
	CHECK_FALSE(equalEdges.Ok);
	CHECK(equalEdges.Code == Status::UnsupportedExecution);
	CHECK(equalEdges.Port == "softness");
}

TEST_CASE("Scratch Noise output depth and mask use shared storage semantics", "[source_scratch_noise]") {
	const Image mask = FloatImage(1, 1, {{.5, .5, .5, .2}});
	for (int64_t depth = 2; depth <= 8; ++depth) {
		CAPTURE(depth);
		const auto run = Scratch({{"attribute_color_depth", EnumValue{depth}}}, {{"mask", &mask}});
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto expectedFormat = SourceSurfaceFormat(depth);
		REQUIRE(expectedFormat.has_value());
		CHECK(run.Output().Format == *expectedFormat);
		CHECK(run.Output().Width == 8);
		CHECK(run.Output().Height == 6);
		SurfacePixel masked = Pixel(run.Output(), 3, 2);
		if (depth < 6)
			CHECK(masked[3] < 1.0);
		else
			CHECK(masked[3] == 1.0);
	}
}

TEST_CASE("Scratch Noise octave conversion rounds half even", "[source_scratch_noise]") {
	const auto two = Scratch({{"octaves", int64_t{2}}});
	const auto twoPointFive = Scratch({{"octaves", 2.5}});
	const auto four = Scratch({{"octaves", int64_t{4}}});
	const auto threePointFive = Scratch({{"octaves", 3.5}});
	REQUIRE(two.Ok);
	REQUIRE(twoPointFive.Ok);
	REQUIRE(four.Ok);
	REQUIRE(threePointFive.Ok);
	CHECK(two.Output().Pixels == twoPointFive.Output().Pixels);
	CHECK(four.Output().Pixels == threePointFive.Output().Pixels);
}

TEST_CASE("Scratch Noise rejects octave counts outside signed 32 bit range", "[source_scratch_noise]") {
	for (double octaves : {2147483648.0, -2147483649.0, std::numeric_limits<double>::infinity()}) {
		CAPTURE(octaves);
		const auto run = Scratch({{"octaves", octaves}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "octaves");
	}
}

TEST_CASE("Scratch Noise skips unused controls when octave count is nonpositive", "[source_scratch_noise]") {
	const auto empty = Scratch(
		{{"octaves", int64_t{0}},
		 {"scale", Vector2{0, 0}},
		 {"thickness_mapped", false},
		 {"wavyness_mapped", false},
		 {"softness_mapped", false}},
		{},
		false
	);
	INFO(empty.Port << ": " << empty.Message);
	CheckPixel(empty, {0, 0, 0, 1}, 3, 2);
}

TEST_CASE("Scratch Noise clears uncovered sprites without reading shader inputs", "[source_scratch_noise]") {
	const auto empty = Scratch(
		{{"dimension", Vector2{.2, .2}},
		 {"dimension_unit", EnumValue{0}},
		 {"scale", Vector2{0, 0}},
		 {"thickness_mapped", false},
		 {"wavyness_mapped", false},
		 {"softness_mapped", false}},
		{},
		false
	);
	INFO(empty.Port << ": " << empty.Message);
	REQUIRE(empty.Ok);
	CHECK(empty.Output().Width == 1);
	CHECK(empty.Output().Height == 1);
	CheckPixel(empty, {0, 0, 0, 0});
}

TEST_CASE("Scratch Noise refuses active raw Atlas samplers", "[source_scratch_noise]") {
	for (const std::string_view port : {"uv_map", "mask", "thickness_map", "wavyness_map", "softness_map"}) {
		const auto run = Scratch({{port, AtlasValue{}}});
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
	}
}

TEST_CASE("Scratch Noise validates output and mask storage before publishing", "[source_scratch_noise]") {
	const Image hugeAlpha = FloatImage(1, 1, {{.5, .2, 0, 3.0e38}});
	const Image largeMask = FloatImage(1, 1, {{10, 10, 10, 1}});
	const auto masked = Scratch({{"uv_mix", 0.0}}, {{"uv_map", &hugeAlpha}, {"mask", &largeMask}});
	CHECK_FALSE(masked.Ok);
	CHECK(masked.Code == Status::InvalidValue);
	CHECK(masked.Port == "mask");
	CHECK(masked.Images.empty());

	const Image halfOverflowAlpha = FloatImage(1, 1, {{.5, .2, 0, 1.0e6}});
	const auto half =
		Scratch({{"uv_mix", 0.0}, {"attribute_color_depth", EnumValue{4}}}, {{"uv_map", &halfOverflowAlpha}});
	CHECK_FALSE(half.Ok);
	CHECK(half.Code == Status::InvalidValue);
	CHECK(half.Port == "surface_out");
	CHECK(half.Images.empty());
}
