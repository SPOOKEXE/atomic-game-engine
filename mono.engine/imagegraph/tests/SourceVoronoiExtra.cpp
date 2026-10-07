#include "../src/AtlasPayload.hpp"
#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_voronoi_extra")
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
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	NodeRun VoronoiExtra(
		std::initializer_list<std::pair<std::string_view, Value>> extra = {},
		std::initializer_list<std::pair<std::string_view, const Image *>> images = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{4, 4}},
			{"dimension_unit", EnumValue{0}},
			{"seed", 17.25},
			{"mode", EnumValue{0}},
			{"progress", 0.0},
			{"parameter_a", 0.0},
			{"position", Vector2{0, 0}},
			{"position_unit", EnumValue{0}},
			{"rotation", 0.0},
			{"scale", Vector2{4, 4}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"tile", true},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : extra) {
			const auto current = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (current == values.end())
				values.emplace_back(port, value);
			else
				current->second = value;
		}
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.voronoi_extra");
		const auto executor = detail::FindExecutor("pc.voronoi_extra");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.voronoi_extra", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
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
	void RequireSuccess(const NodeRun &run) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
	}
	void CheckGray(const NodeRun &run, double expected, uint32_t x = 0, uint32_t y = 0) {
		RequireSuccess(run);
		const auto pixel = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == Catch::Approx(expected).margin(5e-5));
		CHECK(pixel[3] == Catch::Approx(1).margin(1e-6));
	}
	void SamePixels(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	}
	void RequireDifferentPixels(const Image &actual, const Image &expected) {
		CHECK(actual.Pixels != expected.Pixels);
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
	ImageArray DrawArray(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return images;
	}
}

TEST_CASE("Voronoi Extra Block and Triangle modes render tiled and free patterns", "[source_voronoi_extra]") {
	CheckGray(
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"mode", EnumValue{0}}, {"tile", false}}),
		.23397472500801086
	);
	CheckGray(
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"mode", EnumValue{0}}, {"tile", true}}),
		.23397472500801086
	);
	CheckGray(
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"mode", EnumValue{1}}, {"tile", false}}),
		.24658939242362976
	);
	CheckGray(
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"mode", EnumValue{1}}, {"tile", true}}),
		.24658939242362976
	);
	CheckGray(
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"mode", EnumValue{2}}, {"tile", false}}),
		.24658939242362976
	);
	const NodeRun blockTiled = VoronoiExtra({{"mode", EnumValue{0}}, {"tile", true}});
	const NodeRun blockFree = VoronoiExtra({{"mode", EnumValue{0}}, {"tile", false}});
	const NodeRun triangleTiled = VoronoiExtra({{"mode", EnumValue{1}}, {"tile", true}});
	const NodeRun triangleFree = VoronoiExtra({{"mode", EnumValue{1}}, {"tile", false}});
	for (const NodeRun *run : {&blockTiled, &blockFree, &triangleTiled, &triangleFree})
		RequireSuccess(*run);
	SamePixels(blockTiled.Output(), blockFree.Output());
	RequireDifferentPixels(triangleTiled.Output(), triangleFree.Output());
	const NodeRun scalarModeTwo = VoronoiExtra({{"mode", EnumValue{2}}});
	const NodeRun triangle = VoronoiExtra({{"mode", EnumValue{1}}});
	RequireSuccess(scalarModeTwo);
	RequireSuccess(triangle);
	SamePixels(scalarModeTwo.Output(), triangle.Output());
}

TEST_CASE("Voronoi Extra off-center Block and Triangle match independent samples", "[source_voronoi_extra]") {
	const Image uv = FloatPixel({.3125, .3125, 0, 1});
	constexpr std::array<std::array<double, 2>, 2> expected{
		{{.8935492038726807, .8935492038726807}, {{.21174269914627075, .32593002915382385}}}
	};
	for (int64_t mode = 0; mode < 2; ++mode)
		for (int64_t tile = 0; tile < 2; ++tile)
			CheckGray(
				VoronoiExtra(
					{{"dimension", Vector2{3, 2}},
					 {"mode", EnumValue{mode}},
					 {"tile", tile != 0},
					 {"seed", -17.25},
					 {"progress", .375},
					 {"parameter_a", .65},
					 {"position", Vector2{.1, -.2}},
					 {"scale", Vector2{9.25, 11.75}},
					 {"rotation", 37.0}},
					{{"uv_map", &uv}}
				),
				expected[size_t(mode)][size_t(tile)]
			);
}

TEST_CASE("Voronoi Extra tiled Triangle quarter-turn samples match shader values", "[source_voronoi_extra]") {
	const Image uv = FloatPixel({.375, .375, 0, 1});
	constexpr std::array<double, 10> rotations{-1, 0, 89, 90, 179, 180, 269, 270, 359, 360};
	constexpr std::array<double, 10> expected{
		.07383057475090027,
		.07563603669404984,
		.07563603669404984,
		.3004789650440216,
		.3004789650440216,
		.09798707067966461,
		.09798707067966461,
		.07383029907941818,
		.07383029907941818,
		.07563657313585281
	};
	for (size_t index = 0; index < rotations.size(); ++index)
		CheckGray(
			VoronoiExtra(
				{{"dimension", Vector2{3, 2}},
				 {"mode", EnumValue{1}},
				 {"tile", true},
				 {"seed", 31.25},
				 {"progress", -.2},
				 {"scale", Vector2{9, 13}},
				 {"rotation", rotations[index]}},
				{{"uv_map", &uv}}
			),
			expected[index]
		);
}

TEST_CASE("Voronoi Extra fractional dimensions apply ordered levels", "[source_voronoi_extra]") {
	constexpr std::array<double, 2> expected{.3222728669643402, -.5362189412117004};
	for (int64_t mode = 0; mode < 2; ++mode)
		CheckGray(
			VoronoiExtra(
				{{"dimension", Vector2{1.6, 2.6}},
				 {"mode", EnumValue{mode}},
				 {"tile", false},
				 {"scale", Vector2{5.5, 7.25}},
				 {"progress", .25},
				 {"parameter_a", -.4},
				 {"level_in", Vector2{.2, .8}},
				 {"level_out", Vector2{-.25, 1.25}}}
			),
			expected[size_t(mode)],
			0,
			1
		);
}

TEST_CASE(
	"Voronoi Extra UV mapping flips green, mixes coordinates, and preserves sampler alpha",
	"[source_voronoi_extra]"
) {
	const Image uv = FloatPixel({.25, .2, 0, .5});
	const Image flippedEquivalent = FloatPixel({.25, .8, 0, .5});
	const NodeRun plain = VoronoiExtra({{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}});
	const NodeRun mapped =
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}}, {{"uv_map", &uv}});
	const NodeRun zeroMix = VoronoiExtra(
		{{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}, {"uv_mix", 0.0}}, {{"uv_map", &uv}}
	);
	const NodeRun unflippedEquivalent = VoronoiExtra(
		{{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}}, {{"uv_map", &flippedEquivalent}}
	);
	for (const NodeRun *run : {&plain, &mapped, &zeroMix, &unflippedEquivalent})
		RequireSuccess(*run);
	RequireDifferentPixels(plain.Output(), mapped.Output());
	const auto mappedPixel = Pixel(mapped.Output());
	const auto unflippedPixel = Pixel(unflippedEquivalent.Output());
	CHECK(mappedPixel[0] == Catch::Approx(.30000004172325134).margin(5e-5));
	CHECK(unflippedPixel[0] == Catch::Approx(.4912499785423279).margin(5e-5));
	CHECK(mappedPixel[3] == Catch::Approx(.5).margin(1e-6));
	const auto plainPixel = Pixel(plain.Output());
	const auto mixedPixel = Pixel(zeroMix.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(mixedPixel[channel] == Catch::Approx(plainPixel[channel]).margin(1e-6));
	CHECK(mixedPixel[3] == Catch::Approx(plainPixel[3] * .5).margin(1e-6));
}

TEST_CASE("Voronoi Extra levels and mask preserve pixel staging", "[source_voronoi_extra]") {
	const NodeRun base = VoronoiExtra({{"dimension", Vector2{1, 1}}, {"tile", false}});
	const NodeRun leveled =
		VoronoiExtra({{"dimension", Vector2{1, 1}}, {"tile", false}, {"level_out", Vector2{.2, .8}}});
	RequireSuccess(base);
	RequireSuccess(leveled);
	const auto before = Pixel(base.Output());
	const auto afterLevel = Pixel(leveled.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(afterLevel[channel] == Catch::Approx(.2 + .6 * before[channel]).margin(5e-5));

	const Image mask = FloatPixel({.1, .2, .3, .5});
	const NodeRun masked = VoronoiExtra({{"dimension", Vector2{1, 1}}, {"tile", false}}, {{"mask", &mask}});
	const NodeRun alphaOnly = VoronoiExtra(
		{{"dimension", Vector2{1, 1}}, {"tile", false}, {"mask_alpha_only", true}}, {{"mask", &mask}}
	);
	RequireSuccess(masked);
	RequireSuccess(alphaOnly);
	SamePixels(masked.Output(), alphaOnly.Output());
	Image scratch{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
	SurfacePixel expectedMask = before;
	const double maskMean = (.1 + .2 + .3) / 3. * .5;
	expectedMask[3] *= maskMean;
	REQUIRE(StoreSurfacePixel(scratch, 0, 0, expectedMask));
	const auto storedMask = Pixel(scratch);
	const auto afterMask = Pixel(masked.Output());
	for (size_t channel = 0; channel < 4; ++channel)
		CHECK(afterMask[channel] == Catch::Approx(storedMask[channel]).margin(1e-6));
}

TEST_CASE("Voronoi Extra preserves all native output formats", "[source_voronoi_extra]") {
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
		const NodeRun run = VoronoiExtra({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(run);
		CHECK(run.Output().Format == formats[size_t(depth - 2)]);
	}
}

TEST_CASE("Voronoi Extra linked Vector2 projections match direct controls", "[source_voronoi_extra]") {
	for (const std::string_view port : {"position", "scale"}) {
		auto surface = Graph("pc.voronoi_extra", {4, 3});
		surface.Nodes.push_back(Solid("numeric", {2, 1}, {255, 255, 255, 255}));
		surface.Links = {{"numeric", "surface_out", "generator", std::string(port)}};
		Set(surface, "position_unit", EnumValue{0});
		auto direct = Graph("pc.voronoi_extra", {4, 3});
		Set(direct, std::string(port), Vector2{2, 1});
		Set(direct, "position_unit", EnumValue{0});
		SamePixels(Draw(surface), Draw(direct));

		auto surfaceArray = Graph("pc.voronoi_extra", {4, 3});
		SurfaceRows(surfaceArray, port);
		Set(surfaceArray, "position_unit", EnumValue{0});
		auto arrayDirect = Graph("pc.voronoi_extra", {4, 3});
		Set(arrayDirect, std::string(port), Vector2{1, 1});
		Set(arrayDirect, "position_unit", EnumValue{0});
		SamePixels(Draw(surfaceArray), Draw(arrayDirect));
	}
}

TEST_CASE("Voronoi Extra dimension SurfaceArrays retain varying projection rows", "[source_voronoi_extra]") {
	auto varyingSurfaces = Graph("pc.voronoi_extra", {1, 1});
	SurfaceRows(varyingSurfaces, "dimension");
	Set(varyingSurfaces, "dimension_unit", EnumValue{0});
	auto varyingValues = Graph("pc.voronoi_extra", {1, 1});
	varyingValues.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{2, 1}, Vector2{3, 2}));
	varyingValues.Links = {{"dimensions", "array", "generator", "dimension"}};
	Set(varyingValues, "dimension_unit", EnumValue{0});
	const ImageArray surfaceRows = DrawArray(varyingSurfaces);
	const ImageArray valueRows = DrawArray(varyingValues);
	REQUIRE(surfaceRows.Images.size() == 2);
	REQUIRE(valueRows.Images.size() == 2);
	for (size_t index = 0; index < 2; ++index)
		SamePixels(surfaceRows.Images[index], valueRows.Images[index]);

	auto equalSurfaces = Graph("pc.voronoi_extra", {1, 1});
	equalSurfaces.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
	equalSurfaces.Nodes.push_back(Solid("second", {2, 1}, {255, 255, 255, 255}));
	Node list{"surfaces", "value.array", "", {}, {}};
	list.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	equalSurfaces.Nodes.push_back(std::move(list));
	equalSurfaces.Links = {
		{"first", "surface_out", "surfaces", "first"},
		{"second", "surface_out", "surfaces", "second"},
		{"surfaces", "array", "generator", "dimension"}
	};
	Set(equalSurfaces, "dimension_unit", EnumValue{0});
	auto scalar = Graph("pc.voronoi_extra", {1, 1});
	Set(scalar, "dimension", Vector2{2, 1});
	Set(scalar, "dimension_unit", EnumValue{0});
	SamePixels(Draw(equalSurfaces), Draw(scalar));
}

TEST_CASE("Voronoi Extra clears pixels outside the raw sprite footprint", "[source_voronoi_extra]") {
	const NodeRun clear = VoronoiExtra({{"dimension", Vector2{.2, .2}}});
	RequireSuccess(clear);
	CHECK(clear.Output().Width == 1);
	CHECK(clear.Output().Height == 1);
	const auto pixel = Pixel(clear.Output());
	CHECK(pixel[0] == 0);
	CHECK(pixel[1] == 0);
	CHECK(pixel[2] == 0);
	CHECK(pixel[3] == 0);
}

TEST_CASE("Voronoi Extra refuses invalid levels and unresolved seed", "[source_voronoi_extra]") {
	const NodeRun equalLevels = VoronoiExtra({{"level_in", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevels.Ok);
	CHECK(equalLevels.Code == Status::UnsupportedExecution);
	CHECK(equalLevels.Port == "level_in");
	const NodeRun nonFiniteProgress = VoronoiExtra({{"progress", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonFiniteProgress.Ok);
	CHECK(nonFiniteProgress.Code == Status::InvalidValue);
	CHECK(nonFiniteProgress.Port == "progress");
	const NodeRun missingSeed =
		RunNode("pc.voronoi_extra", {}, {{"dimension", Vector2{1, 1}}, {"mode", EnumValue{1}}});
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
}

TEST_CASE("Voronoi Extra rejects Atlas payloads on active surface inputs", "[source_voronoi_extra]") {
	const AtlasValue atlas = SurfaceAtlas();
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask"}) {
		const NodeRun run = RunNode("pc.voronoi_extra", {}, {{port, atlas}, {"seed", 17.25}});
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}
