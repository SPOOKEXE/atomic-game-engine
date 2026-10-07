#include "../src/AtlasPayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_noise")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
	SurfacePixel Pixel(const Image &image) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
		return pixel;
	}
	AtlasValue AtlasFixture() {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = {1, 1, {0, 0, 0, 255}, 0};
		data.Scale = {1, 1};
		data.Dimension = {1, 1};
		return atlas;
	}
	Document NoiseGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"noise",
			 "pc.noise",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"seed", 17.25},
			  {"attribute_color_depth", EnumValue{5}}}}
		};
		document.Outputs = {{"out", "noise", "surface_out"}};
		return document;
	}
	Image DrawGraph(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image image;
		const auto status = Evaluate(document, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	imagegraph_test::NodeRun Sample(double seed) {
		return RunNode(
			"pc.noise",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", seed},
			 {"attribute_color_depth", EnumValue{5}}}
		);
	}
	imagegraph_test::NodeRun SampleMapped(double seed, double uvMix) {
		const Image uv = FloatPixel({.25, .25, 0, .5});
		return RunNode(
			"pc.noise",
			{{"uv_map", &uv}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"uv_mix", uvMix},
			 {"seed", seed},
			 {"attribute_color_depth", EnumValue{5}}}
		);
	}
}

TEST_CASE("Noise matches independent binary32 shader samples in each colour mode", "[source_noise]") {
	const auto grey = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(grey.Message);
	REQUIRE(grey.Ok);
	REQUIRE(grey.Images.size() == 1);
	CHECK(grey.Images.front().first == "surface_out");
	CHECK(grey.Values.empty());
	CHECK(grey.Output().Format == SurfaceFormat::RGBA32Float);
	const auto greyPixel = Pixel(grey.Output());
	CHECK(greyPixel[0] == Catch::Approx(.65378380).margin(1e-5));
	CHECK(greyPixel[1] == Catch::Approx(.65378380).margin(1e-5));
	CHECK(greyPixel[2] == Catch::Approx(.65378380).margin(1e-5));
	CHECK(greyPixel[3] == 1.0);

	const auto rgb = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"color_mode", EnumValue{1}},
		 {"color_r_range", Vector2{-1, 2}},
		 {"color_g_range", Vector2{.25, .75}},
		 {"color_b_range", Vector2{2, 4}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(rgb.Message);
	REQUIRE(rgb.Ok);
	const auto rgbPixel = Pixel(rgb.Output());
	CHECK(rgbPixel[0] == Catch::Approx(.96135139).margin(1e-5));
	CHECK(rgbPixel[1] == Catch::Approx(.49570465).margin(1e-5));
	CHECK(rgbPixel[2] == Catch::Approx(3.46559143).margin(1e-5));
	CHECK(rgbPixel[3] == 1.0);

	const auto hsv = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"color_mode", EnumValue{2}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(hsv.Message);
	REQUIRE(hsv.Ok);
	const auto hsvPixel = Pixel(hsv.Output());
	CHECK(hsvPixel[0] == Catch::Approx(.37269309).margin(1e-5));
	CHECK(hsvPixel[1] == Catch::Approx(.40052783).margin(1e-5));
	CHECK(hsvPixel[2] == Catch::Approx(.73279572).margin(1e-5));
	CHECK(hsvPixel[3] == 1.0);
}

TEST_CASE("Noise seed interpolation is signed and repeats at the shader hash period", "[source_noise]") {
	for (const auto &[seed, expected] :
		 {std::pair{17.0, .63826752}, std::pair{17.25, .65378380}, std::pair{-17.25, .52038574}}) {
		const auto run = Sample(seed);
		INFO("seed " << seed << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(Pixel(run.Output())[0] == Catch::Approx(expected).margin(1e-5));
	}
	const auto base = Sample(0.0);
	// random() hashes floor(seed)/5000, so mod(..., 100000) repeats every 500,000,000 authored seeds.
	const auto repeated = Sample(500000000.0);
	REQUIRE(base.Ok);
	REQUIRE(repeated.Ok);
	CHECK(Pixel(base.Output())[0] == Catch::Approx(.87026978).margin(1e-5));
	CHECK(Pixel(repeated.Output())[0] == Catch::Approx(.87026978).margin(1e-5));
}

TEST_CASE("Noise level mapping preserves reversed ranges and unclamped HDR values", "[source_noise]") {
	const auto reversed = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_in", Vector2{1, 0}},
		 {"level_out", Vector2{-4, 4}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(reversed.Message);
	REQUIRE(reversed.Ok);
	const auto pixel = Pixel(reversed.Output());
	CHECK(pixel[0] == Catch::Approx(-1.23027039).margin(1e-5));
	CHECK(pixel[1] == Catch::Approx(-1.23027039).margin(1e-5));
	CHECK(pixel[2] == Catch::Approx(-1.23027039).margin(1e-5));
}

TEST_CASE("Noise UV mapping flips Y, retains alpha at zero mix and extrapolates", "[source_noise]") {
	const auto unmixed = SampleMapped(17.25, 0);
	INFO(unmixed.Message);
	REQUIRE(unmixed.Ok);
	const auto original = Pixel(unmixed.Output());
	CHECK(original[0] == Catch::Approx(.65378380).margin(1e-5));
	CHECK(original[3] == Catch::Approx(.5).margin(1e-6));

	const auto extrapolated = SampleMapped(17.25, 2);
	INFO(extrapolated.Message);
	REQUIRE(extrapolated.Ok);
	const auto mapped = Pixel(extrapolated.Output());
	CHECK(mapped[0] == Catch::Approx(.85387421).margin(1e-5));
	CHECK(mapped[1] == Catch::Approx(.85387421).margin(1e-5));
	CHECK(mapped[2] == Catch::Approx(.85387421).margin(1e-5));
	CHECK(mapped[3] == Catch::Approx(.5).margin(1e-6));
}

TEST_CASE(
	"Noise supports every source depth and rounds masked float output through RGBA8", "[source_noise]"
) {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const auto run = RunNode(
			"pc.noise",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO("depth " << depth << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto format = SourceSurfaceFormat(depth);
		REQUIRE(format);
		CHECK(run.Output().Format == *format);
	}

	const Image whiteMask{1, 1, {255, 255, 255, 255}, 0};
	const auto unmasked = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_out", Vector2{.123456, .123456}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const auto masked = RunNode(
		"pc.noise",
		{{"mask", &whiteMask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_out", Vector2{.123456, .123456}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(unmasked.Ok);
	REQUIRE(masked.Ok);
	CHECK(Pixel(unmasked.Output())[0] == Catch::Approx(.123456).margin(1e-7));
	CHECK(Pixel(masked.Output())[0] == Catch::Approx(31. / 255).margin(1e-7));
	CHECK(masked.Output().Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE("Noise Level Out projects linked source tuples and surface dimensions", "[source_noise]") {
	const auto compare = [&](Document linked, Vector2 directRange) {
		Document direct = NoiseGraph();
		direct.Nodes.front().Values.push_back({"level_out", directRange});
		const auto actual = DrawGraph(linked);
		const auto expected = DrawGraph(direct);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	};

	for (const auto &[values, expected] :
		 {std::pair{std::vector<double>{.75}, Vector2{.75, 0}},
		  std::pair{std::vector<double>{.75, .25, .9}, Vector2{.75, .25}}}) {
		Document linked = NoiseGraph();
		Node array{"range", "pc.array", "", {}, {}, {}};
		for (size_t index = 0; index < values.size(); ++index)
			array.DynamicInputs.push_back(
				{"input_" + std::to_string(index), ValueType::Scalar, values[index]}
			);
		linked.Nodes.push_back(std::move(array));
		linked.Links = {{"range", "array", "noise", "level_out"}};
		compare(std::move(linked), expected);
	}

	Document scalar = NoiseGraph();
	scalar.Nodes.push_back({"number", "pc.number_simple", "", {}, {{"value", .75}}});
	scalar.Links = {{"number", "number", "noise", "level_out"}};
	compare(std::move(scalar), {.75, .75});

	Document surface = NoiseGraph();
	surface.Nodes.push_back(
		{"surface",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{3, 2}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 0, 0, 255}}}}
	);
	surface.Links = {{"surface", "surface_out", "noise", "level_out"}};
	compare(std::move(surface), {3, 2});
}

TEST_CASE("Noise clamps shader and mask values at the RGBA8 storage boundaries", "[source_noise]") {
	const Image uv = FloatPixel({.25, .25, 0, 1e30});
	const Image mask = FloatPixel({1e30, 1e30, 1e30, 1});
	const auto run = RunNode(
		"pc.noise",
		{{"uv_map", &uv}, {"mask", &mask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_out", Vector2{4, 4}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}

TEST_CASE("Noise dimensions use half-even units and keep zero raw dimensions finite", "[source_noise]") {
	const auto pixels = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{2.5, 3.5}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(pixels.Ok);
	CHECK(pixels.Output().Width == 2);
	CHECK(pixels.Output().Height == 4);

	const auto project = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{.078125, .109375}},
		 {"dimension_unit", EnumValue{1}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(project.Ok);
	CHECK(project.Output().Width == 2);
	CHECK(project.Output().Height == 4);

	const Image mask{8, 4, std::vector<uint8_t>(8 * 4 * 4, 255), 0};
	const auto maskUnits = RunNode(
		"pc.noise",
		{{"mask", &mask}},
		{{"dimension", Vector2{.3125, .875}},
		 {"dimension_unit", EnumValue{2}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(maskUnits.Ok);
	CHECK(maskUnits.Output().Width == 2);
	CHECK(maskUnits.Output().Height == 4);

	const auto zero = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{0, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(zero.Message);
	REQUIRE(zero.Ok);
	CHECK(zero.Output().Width == 1);
	CHECK(zero.Output().Height == 1);
	CHECK(FiniteSurfaceSamples(zero.Output()));
}

TEST_CASE("Noise refuses invalid controls and raw Atlas sampler bindings", "[source_noise]") {
	const auto missingSeed = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");

	const auto equalLevels = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}},
		 {"level_in", Vector2{.5, .5}}}
	);
	CHECK_FALSE(equalLevels.Ok);
	CHECK(equalLevels.Code == Status::UnsupportedExecution);
	CHECK(equalLevels.Port == "level_in");

	const auto overflow = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", std::numeric_limits<double>::max()},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Code == Status::InvalidValue);
	CHECK(overflow.Port == "seed");

	const float floatMaximum = std::numeric_limits<float>::max();
	const auto derivedOverflow = RunNode(
		"pc.noise",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"color_mode", EnumValue{1}},
		 {"color_r_range", Vector2{-double(floatMaximum), double(floatMaximum)}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CHECK_FALSE(derivedOverflow.Ok);
	CHECK(derivedOverflow.Code == Status::InvalidValue);
	CHECK(derivedOverflow.Port == "color_r_range");

	const AtlasValue atlasInput = AtlasFixture();
	REQUIRE(detail::ValidAtlasPayload(atlasInput));
	for (const std::string_view port : {"uv_map", "mask"}) {
		const auto atlasRun = RunNode(
			"pc.noise",
			{},
			{{port, atlasInput},
			 {"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(port << ": " << atlasRun.Message);
		CHECK_FALSE(atlasRun.Ok);
		CHECK(atlasRun.Code == Status::UnsupportedExecution);
		CHECK(atlasRun.Port == port);
	}
}
