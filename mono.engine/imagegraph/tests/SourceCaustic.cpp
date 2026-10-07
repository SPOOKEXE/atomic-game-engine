#include "../src/AtlasPayload.hpp"
#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_caustic")
using namespace complex_generator_test;
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;
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
	Image BytePixel(std::array<uint8_t, 4> pixel) {
		return Image{1, 1, {pixel[0], pixel[1], pixel[2], pixel[3]}, 0, SurfaceFormat::RGBA8Unorm};
	}
	NodeRun Caustic(
		double seed = 17.25,
		int64_t detail = 1,
		double progress = 0,
		double intensity = 1,
		Vector2 dimension = {1, 1},
		Vector2 position = {0, 0},
		Vector2 scale = {.5, .5},
		int64_t dimensionUnit = 0,
		int64_t positionUnit = 0,
		int64_t scaleUnit = 0,
		bool progressMapped = true,
		bool intensityMapped = true
	) {
		return RunNode(
			"pc.caustic",
			{},
			{{"dimension", dimension},
			 {"dimension_unit", EnumValue{dimensionUnit}},
			 {"position", position},
			 {"position_unit", EnumValue{positionUnit}},
			 {"scale", scale},
			 {"scale_unit", EnumValue{scaleUnit}},
			 {"seed", seed},
			 {"detail", detail},
			 {"progress", progress},
			 {"progress_mapped", progressMapped},
			 {"progress_map_range", Vector2{progress, progress}},
			 {"intensity", intensity},
			 {"intensity_mapped", intensityMapped},
			 {"intensity_map_range", Vector2{intensity, intensity}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
	}
	Document CausticGraph(Vector2 dimension = {1, 1}) {
		Document document = Graph("pc.caustic", dimension);
		Set(document, "seed", 17.25);
		Set(document, "attribute_color_depth", EnumValue{5});
		Set(document, "intensity_mapped", true);
		Set(document, "intensity_map_range", Vector2{1, 1});
		Set(document, "progress_mapped", true);
		Set(document, "progress_map_range", Vector2{0, 0});
		Set(document, "position_unit", EnumValue{0});
		Set(document, "scale_unit", EnumValue{0});
		return document;
	}
	Image DrawCaustic(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto evaluated = Evaluate(document, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return image;
	}
	NodeRun CausticMasked(Vector2 dimension, const Image &mask) {
		return RunNode(
			"pc.caustic",
			{{"mask", &mask}},
			{{"dimension", dimension},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{0, 0}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{.5, .5}},
			 {"scale_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"detail", int64_t{1}},
			 {"progress", 0.0},
			 {"progress_mapped", true},
			 {"progress_map_range", Vector2{0, 0}},
			 {"intensity", 1.0},
			 {"intensity_mapped", true},
			 {"intensity_map_range", Vector2{1, 1}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
	}
	void CheckReference(const NodeRun &run, double expected) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.Output().Format == SurfaceFormat::RGBA32Float);
		const SurfacePixel pixel = Pixel(run.Output());
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == Catch::Approx(expected).margin(5e-5));
		CHECK(pixel[3] == Catch::Approx(1).margin(1e-6));
	}
	void CheckSameImage(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	}
	AtlasValue SurfaceAtlas() {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = {1, 1, {64, 128, 192, 255}, 0};
		data.Scale = {1, 1};
		data.Dimension = {1, 1};
		return atlas;
	}
}

TEST_CASE("Caustic matches independent binary32 shader samples", "[source_caustic]") {
	CheckReference(Caustic(0, 1), .12940756976604462);
	CheckReference(Caustic(17.25, 1), .03733130916953087);
	CheckReference(Caustic(17.25, 3, .75), .055738504976034164);
	CheckReference(Caustic(-17.25, 2, -.5), .08367660641670227);
}

TEST_CASE("Caustic detail zero and negative preserve the source empty loop", "[source_caustic]") {
	for (const int64_t detail : {0, -1, -3}) {
		const NodeRun run = Caustic(17.25, detail);
		INFO("detail " << detail << ": " << run.Message);
		REQUIRE(run.Ok);
		const SurfacePixel pixel = Pixel(run.Output());
		CHECK(pixel[0] == 0);
		CHECK(pixel[1] == 0);
		CHECK(pixel[2] == 0);
		CHECK(pixel[3] == Catch::Approx(1).margin(1e-6));
	}
	const NodeRun zeroDetailZeroScale =
		Caustic(17.25, 0, 0, 1, {1, 1}, {0, 0}, {0, 0}, 0, 0, 0, false, false);
	REQUIRE(zeroDetailZeroScale.Ok);
	const SurfacePixel zeroScalePixel = Pixel(zeroDetailZeroScale.Output());
	CHECK(zeroScalePixel[0] == 0);
	CHECK(zeroScalePixel[1] == 0);
	CHECK(zeroScalePixel[2] == 0);
	CHECK(zeroScalePixel[3] == Catch::Approx(1).margin(1e-6));
}

TEST_CASE("Caustic UV flips green and keeps map alpha independent of UV Mix", "[source_caustic]") {
	const Image uv = FloatPixel({.25, .25, 0, 1});
	const NodeRun flipped = RunNode(
		"pc.caustic",
		{{"uv_map", &uv}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"uv_mix", 1.0},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"progress", 0.0},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 0}},
		 {"intensity", 1.0},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{1, 1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	CheckReference(flipped, .08700716495513916);

	const Image halfAlphaUv = FloatPixel({.25, .25, 0, .5});
	const NodeRun unmapped = Caustic();
	const NodeRun zeroMix = RunNode(
		"pc.caustic",
		{{"uv_map", &halfAlphaUv}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"uv_mix", 0.0},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"progress", 0.0},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 0}},
		 {"intensity", 1.0},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{1, 1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(unmapped.Ok);
	REQUIRE(zeroMix.Ok);
	const SurfacePixel original = Pixel(unmapped.Output());
	const SurfacePixel mixed = Pixel(zeroMix.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(mixed[channel] == original[channel]);
	CHECK(mixed[3] == Catch::Approx(original[3] * .5).margin(1e-6));
}

TEST_CASE("Caustic mapped means use RGB range interpolation and ignore map alpha", "[source_caustic]") {
	const Image map = BytePixel({255, 0, 0, 0});
	const NodeRun mapped = RunNode(
		"pc.caustic",
		{{"intensity_map", &map}, {"progress_map", &map}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{0, 3}},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 2}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const double mean = 1. / 3.;
	const NodeRun direct = Caustic(17.25, 1, 2 * mean, 3 * mean);
	REQUIRE(mapped.Ok);
	REQUIRE(direct.Ok);
	CheckSameImage(mapped.Output(), direct.Output());

	const Image opaqueMap = BytePixel({255, 0, 0, 255});
	const NodeRun opaqueMapped = RunNode(
		"pc.caustic",
		{{"intensity_map", &opaqueMap}, {"progress_map", &opaqueMap}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{0, 3}},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 2}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(opaqueMapped.Ok);
	CheckSameImage(opaqueMapped.Output(), mapped.Output());

	const NodeRun equalRanges = RunNode(
		"pc.caustic",
		{{"intensity_map", &map}, {"progress_map", &map}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{1.25, 1.25}},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{-.5, -.5}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const NodeRun equalDirect = Caustic(17.25, 1, -.5, 1.25);
	REQUIRE(equalRanges.Ok);
	REQUIRE(equalDirect.Ok);
	CheckSameImage(equalRanges.Output(), equalDirect.Output());
}

TEST_CASE("Caustic retains raw fractional dimension UVs and half-even coverage", "[source_caustic]") {
	const NodeRun fractional = RunNode(
		"pc.caustic",
		{},
		{{"dimension", Vector2{2.5, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"progress", 0.0},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 0}},
		 {"intensity", 1.0},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{1, 1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(fractional.Ok);
	CHECK(fractional.Output().Width == 2);
	CHECK(fractional.Output().Height == 1);
	const SurfacePixel first = Pixel(fractional.Output());
	const SurfacePixel second = [&] {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(fractional.Output(), 1, 0, pixel));
		return pixel;
	}();
	CHECK(first[0] == Catch::Approx(.04147619754076004).margin(5e-5));
	CHECK(second[0] == Catch::Approx(.13175255060195923).margin(5e-5));

	auto halfGraph = CausticGraph({.5, 1});
	Set(halfGraph, "scale", Vector2{0, 0});
	Set(halfGraph, "progress_mapped", false);
	Set(halfGraph, "progress_map_range", Vector2{7, -6});
	Set(halfGraph, "intensity_mapped", false);
	Set(halfGraph, "intensity_map_range", Vector2{9, -8});
	const Image half = DrawCaustic(halfGraph);
	REQUIRE(half.Width == 1);
	REQUIRE(half.Height == 1);
	CHECK(half.Pixels == std::vector<uint8_t>(16, 0));
	const Image oneAndHalf = DrawCaustic(CausticGraph({1.5, 1}));
	REQUIRE(oneAndHalf.Width == 2);
	REQUIRE(oneAndHalf.Height == 1);
	CHECK(
		std::vector<uint8_t>(oneAndHalf.Pixels.begin() + 16, oneAndHalf.Pixels.end()) ==
		std::vector<uint8_t>(16, 0)
	);
	for (const double rawWidth : {0., -1.}) {
		auto uncovered = CausticGraph({rawWidth, 1});
		Set(uncovered, "scale", Vector2{0, 0});
		Set(uncovered, "progress_mapped", false);
		Set(uncovered, "progress_map_range", Vector2{7, -6});
		Set(uncovered, "intensity_mapped", false);
		Set(uncovered, "intensity_map_range", Vector2{9, -8});
		const Image empty = DrawCaustic(uncovered);
		REQUIRE(empty.Pixels.size() == size_t(empty.Width) * empty.Height * 16);
		CHECK(empty.Pixels == std::vector<uint8_t>(empty.Pixels.size(), 0));
	}
}

TEST_CASE("Caustic Pixel and Project dimensions use half-even allocation", "[source_caustic]") {
	auto pixel = CausticGraph({2.5, 3.5});
	const Image expected = DrawCaustic(pixel);
	REQUIRE(expected.Width == 2);
	REQUIRE(expected.Height == 4);
	pixel.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Set(pixel, "dimension", Vector2{1.25, 1.75});
	Set(pixel, "dimension_unit", EnumValue{1});
	CheckSameImage(DrawCaustic(pixel), expected);
}

TEST_CASE("Caustic Mask dimensions and position and scale units resolve consistently", "[source_caustic]") {
	auto maskedDimension = CausticGraph({.5, .5});
	Set(maskedDimension, "dimension_unit", EnumValue{2});
	maskedDimension.Nodes.push_back(Solid("mask", {8, 4}, {255, 255, 255, 255}));
	maskedDimension.Links = {{"mask", "surface_out", "generator", "mask"}};
	const Image maskSized = DrawCaustic(maskedDimension);
	CHECK(maskSized.Width == 4);
	CHECK(maskSized.Height == 2);

	const NodeRun pixelUnits = Caustic(17.25, 1, 0, 1, {4, 3}, {1, 1.5}, {2, 1.5}, 0, 0, 0);
	const NodeRun referenceUnits = Caustic(17.25, 1, 0, 1, {4, 3}, {.25, .5}, {.5, .5}, 0, 1, 1);
	REQUIRE(pixelUnits.Ok);
	REQUIRE(referenceUnits.Ok);
	CheckSameImage(pixelUnits.Output(), referenceUnits.Output());
}

TEST_CASE("Caustic projects linked Surface and SurfaceArray position and scale", "[source_caustic]") {
	for (const std::string_view port : {"position", "scale"}) {
		auto linkedSurface = CausticGraph({4, 3});
		linkedSurface.Nodes.push_back(Solid("numeric", {2, 1}, {0, 0, 0, 0}));
		linkedSurface.Links = {{"numeric", "surface_out", "generator", std::string(port)}};
		auto directSurface = CausticGraph({4, 3});
		Set(directSurface, std::string(port), Vector2{2, 1});
		CheckSameImage(DrawCaustic(linkedSurface), DrawCaustic(directSurface));

		auto linkedArray = CausticGraph({4, 3});
		SurfaceRows(linkedArray, port);
		auto directArray = CausticGraph({4, 3});
		Set(directArray, std::string(port), Vector2{1, 1});
		Set(directArray, std::string(port) + "_unit", EnumValue{0});
		CheckSameImage(DrawCaustic(linkedArray), DrawCaustic(directArray));
	}
}

TEST_CASE("Caustic supports seven native depths and masks through RGBA8 scratch", "[source_caustic]") {
	constexpr std::array<SurfaceFormat, 7> formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const NodeRun run = RunNode(
			"pc.caustic",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{0, 0}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{.5, .5}},
			 {"scale_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"detail", int64_t{1}},
			 {"progress", 0.0},
			 {"progress_mapped", true},
			 {"progress_map_range", Vector2{0, 0}},
			 {"intensity", 1.0},
			 {"intensity_mapped", true},
			 {"intensity_map_range", Vector2{1, 1}},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO("depth " << depth << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == formats[size_t(depth - 2)]);
		const auto format = DescribeSurfaceFormat(run.Output().Format);
		REQUIRE(format);
		CHECK(run.Output().Pixels.size() == format->BytesPerPixel);
	}

	const Image whiteMask = BytePixel({255, 255, 255, 255});
	const NodeRun unmasked = Caustic();
	const NodeRun masked = RunNode(
		"pc.caustic",
		{{"mask", &whiteMask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{.5, .5}},
		 {"scale_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"detail", int64_t{1}},
		 {"intensity", 1.0},
		 {"intensity_mapped", true},
		 {"intensity_map_range", Vector2{1, 1}},
		 {"progress", 0.0},
		 {"progress_mapped", true},
		 {"progress_map_range", Vector2{0, 0}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(unmasked.Ok);
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Format == SurfaceFormat::RGBA32Float);
	const SurfacePixel source = Pixel(unmasked.Output());
	Image rgba8{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
	REQUIRE(StoreSurfacePixel(rgba8, 0, 0, source));
	const SurfacePixel quantized = Pixel(rgba8);
	const SurfacePixel actual = Pixel(masked.Output());
	for (size_t channel = 0; channel < 4; ++channel)
		CHECK(actual[channel] == Catch::Approx(quantized[channel]).margin(1e-6));
}

TEST_CASE("Caustic array rows apply mask alpha through RGBA8 scratch", "[source_caustic]") {
	auto graph = CausticGraph({1, 1});
	graph.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{2, 1}));
	graph.Nodes.push_back(Solid("mask", {2, 1}, {255, 255, 255, 128}));
	graph.Links = {
		{"dimensions", "array", "generator", "dimension"}, {"mask", "surface_out", "generator", "mask"}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
	ImageArray withMask;
	REQUIRE(EvaluateArray(graph, plan, "out", {}, withMask, diagnostic) == Status::Ok);
	REQUIRE(withMask.Images.size() == 2);
	std::erase_if(graph.Links, [](const Link &link) { return link.ToPort == "mask"; });
	REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
	ImageArray withoutMask;
	REQUIRE(EvaluateArray(graph, plan, "out", {}, withoutMask, diagnostic) == Status::Ok);
	REQUIRE(withoutMask.Images.size() == 2);
	const Image mask{2, 1, {255, 255, 255, 128, 255, 255, 255, 128}, 0, SurfaceFormat::RGBA8Unorm};
	const std::array<Vector2, 2> dimensions{Vector2{1, 1}, Vector2{2, 1}};
	for (size_t index = 0; index < withMask.Images.size(); ++index) {
		const Image &actual = withMask.Images[index];
		const NodeRun standalone = CausticMasked(dimensions[index], mask);
		REQUIRE(standalone.Ok);
		CheckSameImage(actual, standalone.Output());
		CHECK(actual.Pixels != withoutMask.Images[index].Pixels);
	}
}

TEST_CASE("Caustic rejects undefined scale seed and derived shader values", "[source_caustic]") {
	const NodeRun zeroScale = Caustic(17.25, 1, 0, 1, {1, 1}, {0, 0}, {0, .5});
	CHECK_FALSE(zeroScale.Ok);
	CHECK(zeroScale.Port == "scale");
	CHECK(zeroScale.Code == Status::UnsupportedExecution);

	const NodeRun missingSeed = RunNode(
		"pc.caustic",
		{},
		{{"dimension", Vector2{1, 1}}, {"scale", Vector2{.5, .5}}, {"attribute_color_depth", EnumValue{5}}}
	);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Port == "seed");

	const NodeRun nonfiniteScale =
		Caustic(17.25, 1, 0, 1, {1, 1}, {0, 0}, {std::numeric_limits<double>::infinity(), .5});
	CHECK_FALSE(nonfiniteScale.Ok);
	CHECK(nonfiniteScale.Port == "scale");

	const NodeRun nonfiniteIntensity = Caustic(17.25, 1, 0, std::numeric_limits<double>::infinity());
	CHECK_FALSE(nonfiniteIntensity.Ok);
	CHECK(nonfiniteIntensity.Port == "intensity");
}

TEST_CASE("Caustic requires both mapped controls for covered positive detail", "[source_caustic]") {
	const NodeRun progressDisabled = Caustic(17.25, 1, 0, 1, {1, 1}, {0, 0}, {.5, .5}, 0, 0, 0, false, true);
	CHECK_FALSE(progressDisabled.Ok);
	CHECK(progressDisabled.Code == Status::UnsupportedExecution);
	CHECK(progressDisabled.Port == "progress");

	const NodeRun intensityDisabled = Caustic(17.25, 1, 0, 1, {1, 1}, {0, 0}, {.5, .5}, 0, 0, 0, true, false);
	CHECK_FALSE(intensityDisabled.Ok);
	CHECK(intensityDisabled.Code == Status::UnsupportedExecution);
	CHECK(intensityDisabled.Port == "intensity");
}

TEST_CASE("Caustic raw sampler inputs refuse valid Atlas payloads", "[source_caustic]") {
	const AtlasValue atlas = SurfaceAtlas();
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask", "intensity_map", "progress_map"}) {
		const NodeRun run = RunNode(
			"pc.caustic",
			{},
			{{port, atlas},
			 {"seed", 17.25},
			 {"detail", int64_t{1}},
			 {"intensity_mapped", true},
			 {"intensity_map_range", Vector2{1, 1}},
			 {"progress_mapped", true},
			 {"progress_map_range", Vector2{0, 0}}}
		);
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}
