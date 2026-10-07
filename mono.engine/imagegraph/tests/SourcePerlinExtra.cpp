#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/NoiseField.hpp>
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

TEST_SUITE_ID("engine.imagegraph.source_perlin_extra")
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
	NodeRun PerlinExtraWithValues(
		const std::vector<std::pair<std::string_view, Value>> &extra,
		const std::vector<std::pair<std::string_view, const Image *>> &images = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{4, 4}},
			{"dimension_unit", EnumValue{0}},
			{"seed", 17.25},
			{"noise_type", EnumValue{0}},
			{"iteration", int64_t{2}},
			{"tile", true},
			{"parameter_a", .35},
			{"parameter_a_mapped", true},
			{"parameter_a_map_range", Vector2{.35, .35}},
			{"parameter_b", 1.0},
			{"parameter_b_mapped", false},
			{"position", Vector2{0, 0}},
			{"position_unit", EnumValue{0}},
			{"rotation", 0.0},
			{"scale", Vector2{4, 4}},
			{"scale_mapped", true},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"color_mode", EnumValue{0}},
			{"color_r_range", Vector2{0, 1}},
			{"color_g_range", Vector2{0, 1}},
			{"color_b_range", Vector2{0, 1}},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : extra) {
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (found == values.end())
				values.emplace_back(port, value);
			else
				found->second = value;
		}
		int64_t noiseType = 0;
		for (const auto &[port, value] : values)
			if (port == "noise_type")
				if (const auto *choice = std::get_if<EnumValue>(&value)) noiseType = choice->Value;
		if (noiseType > 0) {
			const bool hasMappedOverride = std::any_of(extra.begin(), extra.end(), [](const auto &item) {
				return item.first == "parameter_a_mapped";
			});
			if (!hasMappedOverride) {
				auto &mapped = *std::find_if(values.begin(), values.end(), [](const auto &item) {
					return item.first == "parameter_a_mapped";
				});
				mapped.second = true;
			}
			const auto parameter = std::find_if(values.begin(), values.end(), [](const auto &item) {
				return item.first == "parameter_a";
			});
			Vector2 authored{};
			if (const auto *scalar = std::get_if<double>(&parameter->second))
				authored = {*scalar, *scalar};
			else if (const auto *pair = std::get_if<Vector2>(&parameter->second))
				authored = *pair;
			const auto range = std::find_if(values.begin(), values.end(), [](const auto &item) {
				return item.first == "parameter_a_map_range";
			});
			if (std::none_of(extra.begin(), extra.end(), [](const auto &item) {
					return item.first == "parameter_a_map_range";
				}))
				range->second = authored;
		}
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.perlin_extra");
		const auto executor = detail::FindExecutor("pc.perlin_extra");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.perlin_extra", "", {}, {}};
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
	NodeRun PerlinExtraReference(
		std::initializer_list<std::pair<std::string_view, Value>> overrides = {},
		bool withUv = true,
		std::initializer_list<std::pair<std::string_view, const Image *>> extraImages = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"seed", 17.25},
			{"noise_type", EnumValue{2}},
			{"iteration", int64_t{2}},
			{"tile", true},
			{"parameter_a", Vector2{.35, .35}},
			{"parameter_a_mapped", true},
			{"parameter_a_map_range", Vector2{.35, .35}},
			{"parameter_b", 1.0},
			{"position", Vector2{-.25, .375}},
			{"position_unit", EnumValue{0}},
			{"rotation", 17.0},
			{"scale", Vector2{3, 4}},
			{"scale_mapped", true},
			{"level_in", Vector2{-.2, 1.3}},
			{"level_out", Vector2{.15, .85}},
			{"color_mode", EnumValue{0}},
			{"color_r_range", Vector2{.2, .9}},
			{"color_g_range", Vector2{.1, .7}},
			{"color_b_range", Vector2{.3, .8}},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : overrides) {
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == port;
			});
			if (found == values.end())
				values.emplace_back(port, value);
			else
				found->second = value;
		}
		const Image uv = FloatPixel({.23, .38, 0, .6});
		std::vector<std::pair<std::string_view, const Image *>> images(extraImages);
		if (withUv && std::none_of(images.begin(), images.end(), [](const auto &item) {
				return item.first == "uv_map";
			}))
			images.emplace_back("uv_map", &uv);
		return PerlinExtraWithValues(values, images);
	}
	void RequireSuccess(const NodeRun &run) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
	}
	void SamePixels(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	}
	void CheckReference(const NodeRun &run, SurfacePixel expected, uint32_t x = 0, uint32_t y = 0) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto actual = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(5e-5));
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

TEST_CASE("Perlin Extra modes match independent binary32 shader references", "[source_perlin_extra]") {
	constexpr std::array<SurfacePixel, 21> expected{{{.4062827229, .4062827229, .4062827229, .6000000238},
													 {.4843978882, .3074389994, .4856445491, .6000000238},
													 {.3363384902, .4856445491, .4716676176, .6000000238},
													 {.4166571796, .4166571796, .4166571796, .6000000238},
													 {.4916599989, .3752671778, .5215738416, .6000000238},
													 {.3258443177, .5215738416, .5117796063, .6000000238},
													 {.4500028491, .4500028491, .4500028491, .6000000238},
													 {.5150020123, .3829438090, .5174154639, .6000000238},
													 {.3192744255, .4995802939, .5174154639, .6000000238},
													 {.6192751527, .6192751527, .6192751527, .6000000238},
													 {.6334925890, .4582189023, .5800186992, .6000000238},
													 {.3142431378, .3671443760, .5800186992, .6000000238},
													 {.5502490401, .5502490401, .5502490401, .6000000238},
													 {.5851743221, .4385646284, .4857264459, .6000000238},
													 {.2727040052, .3768620789, .4857264459, .6000000238},
													 {.6021071076, .6021071076, .6021071076, .6000000238},
													 {.6214749813, .3989687860, .5624016523, .6000000238},
													 {.3380209208, .3988616765, .5624016523, .6000000238},
													 {.3711177111, .3711177111, .3711177111, .6000000238},
													 {.4597824216, .1097676754, .3802099526, .6000000238},
													 {.3384751976, .3802099526, .3701391220, .6000000238}}};
	size_t sample = 0;
	for (int64_t type = 0; type < 7; ++type)
		for (int64_t color = 0; color < 3; ++color) {
			CAPTURE(type, color);
			CheckReference(
				PerlinExtraReference({{"noise_type", EnumValue{type}}, {"color_mode", EnumValue{color}}}),
				expected[sample++]
			);
		}
}

TEST_CASE(
	"Perlin Extra tile, edge, and mode branches match independent references", "[source_perlin_extra]"
) {
	constexpr std::array<SurfacePixel, 7> untiled{
		{{.4421685636, .4421685636, .4421685636, .6000000238},
		 {.4443070292, .4443070292, .4443070292, .6000000238},
		 {.3945531547, .3945531547, .3945531547, .6000000238},
		 {.6261458993, .6261458993, .6261458993, .6000000238},
		 {.4021016061, .4021016061, .4021016061, .6000000238},
		 {.4992804229, .4992804229, .4992804229, .6000000238},
		 {.2576812208, .2576812208, .2576812208, .6000000238}}
	};
	for (int64_t type = 0; type < 7; ++type) {
		CAPTURE(type);
		CheckReference(
			PerlinExtraReference({{"noise_type", EnumValue{type}}, {"tile", false}}), untiled[size_t(type)]
		);
	}
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{1}}, {"scale", Vector2{3.25, 2.75}}}),
		{.5140630007, .5140630007, .5140630007, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{6}}, {"scale", Vector2{3.25, 2.75}}}),
		{.2934489548, .2934489548, .2934489548, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{0}}, {"seed", -17.25}}),
		{.3379602134, .3379602134, .3379602134, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{2}}, {"seed", -17.25}}),
		{.5116479993, .5116479993, .5116479993, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{2}}, {"uv_mix", 0.0}}),
		{.4266026914, .4266026914, .4266026914, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{2}}, {"dimension", Vector2{3.6, 2.6}}}, false),
		{.4346243143, .4346243143, .4346243143, 1.0},
		1,
		1
	);
	const Image white = FloatPixel({1, 1, 1, 1});
	CheckReference(
		PerlinExtraReference(
			{{"noise_type", EnumValue{2}}, {"scale", Vector2{3, 5}}}, true, {{"scale_map", &white}}
		),
		{.4801329970, .4801329970, .4801329970, .6000000238}
	);
	CheckReference(
		PerlinExtraReference(
			{{"noise_type", EnumValue{3}}, {"parameter_a", Vector2{.1, .8}}},
			true,
			{{"parameter_a_map", &white}}
		),
		{.5485160947, .5485160947, .5485160947, .6000000238}
	);
	CheckReference(
		PerlinExtraReference(
			{{"noise_type", EnumValue{6}}, {"level_in", Vector2{.5, .5}}, {"level_out", Vector2{8, -8}}}
		),
		{.3711177111, .3711177111, .3711177111, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{2}}, {"parameter_b", -999.0}}),
		{.4500028491, .4500028491, .4500028491, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"iteration", int64_t{0}}}),
		{.2433333546, .2433333546, .2433333546, .6000000238}
	);
	CheckReference(
		PerlinExtraReference({{"noise_type", EnumValue{6}}, {"iteration", int64_t{-1}}}),
		{0, 0, 0, .6000000238}
	);
	CheckReference(
		PerlinExtraReference(
			{{"noise_type", EnumValue{2}},
			 {"tile", false},
			 {"iteration", int64_t{1}},
			 {"dimension", Vector2{1, 1}},
			 {"position", Vector2{.5, .5}},
			 {"scale", Vector2{3e38, 3e38}}},
			false
		),
		{.4478418231, .4478418231, .4478418231, 1.0}
	);
	const NodeRun consumedScaleOverflow = PerlinExtraReference(
		{{"noise_type", EnumValue{2}},
		 {"tile", false},
		 {"iteration", int64_t{2}},
		 {"dimension", Vector2{1, 1}},
		 {"position", Vector2{.5, .5}},
		 {"scale", Vector2{3e38, 3e38}}},
		false
	);
	CHECK_FALSE(consumedScaleOverflow.Ok);
	CHECK(consumedScaleOverflow.Code == Status::InvalidValue);
	CHECK(consumedScaleOverflow.Port == "scale");
}

TEST_CASE("Perlin Extra ignores Parameter B and uses levels except in Vine mode", "[source_perlin_extra]") {
	const NodeRun ignoredA = PerlinExtraWithValues({{"dimension", Vector2{3, 2}}, {"parameter_a", 0.0}});
	const NodeRun ignoredAChanged =
		PerlinExtraWithValues({{"dimension", Vector2{3, 2}}, {"parameter_a", 100.0}});
	RequireSuccess(ignoredA);
	RequireSuccess(ignoredAChanged);
	SamePixels(ignoredA.Output(), ignoredAChanged.Output());
	for (int64_t type = 0; type <= 6; ++type) {
		const NodeRun lowB = PerlinExtraWithValues(
			{{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{type}}, {"parameter_b", -100.0}}
		);
		const NodeRun highB = PerlinExtraWithValues(
			{{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{type}}, {"parameter_b", 100.0}}
		);
		RequireSuccess(lowB);
		RequireSuccess(highB);
		SamePixels(lowB.Output(), highB.Output());
	}
	const NodeRun vine = PerlinExtraWithValues(
		{{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{6}}, {"level_out", Vector2{.2, .8}}}
	);
	const NodeRun vineDefault =
		PerlinExtraWithValues({{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{6}}});
	RequireSuccess(vine);
	RequireSuccess(vineDefault);
	SamePixels(vine.Output(), vineDefault.Output());
	const NodeRun leveled = PerlinExtraWithValues(
		{{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{0}}, {"level_out", Vector2{.2, .8}}}
	);
	const NodeRun unlevelled =
		PerlinExtraWithValues({{"dimension", Vector2{3, 2}}, {"noise_type", EnumValue{0}}});
	RequireSuccess(leveled);
	RequireSuccess(unlevelled);
	CHECK(leveled.Output().Pixels != unlevelled.Output().Pixels);
}

TEST_CASE(
	"Perlin Extra UV mix preserves the original coordinates at zero and applies UV alpha",
	"[source_perlin_extra]"
) {
	const Image uv = FloatPixel({.31, .27, 0, .5});
	const NodeRun plain = PerlinExtraWithValues({{"dimension", Vector2{1, 1}}});
	const NodeRun zeroMix =
		PerlinExtraWithValues({{"dimension", Vector2{1, 1}}, {"uv_mix", 0.0}}, {{"uv_map", &uv}});
	const NodeRun mapped = PerlinExtraWithValues({{"dimension", Vector2{1, 1}}}, {{"uv_map", &uv}});
	for (const NodeRun *run : {&plain, &zeroMix, &mapped})
		RequireSuccess(*run);
	const auto plainPixel = Pixel(plain.Output());
	const auto zeroPixel = Pixel(zeroMix.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(zeroPixel[channel] == Catch::Approx(plainPixel[channel]).margin(1e-6));
	CHECK(zeroPixel[3] == Catch::Approx(plainPixel[3] * .5).margin(1e-6));
	CHECK(Pixel(mapped.Output())[3] == Catch::Approx(.5).margin(1e-6));
}

TEST_CASE("Perlin Extra mask scales alpha and honors requested output formats", "[source_perlin_extra]") {
	const Image mask = FloatPixel({.2, .4, .6, .6});
	const NodeRun masked = PerlinExtraReference({{"noise_type", EnumValue{2}}}, true, {{"mask", &mask}});
	RequireSuccess(masked);
	const auto maskPixel = Pixel(masked.Output());
	CHECK(masked.Output().Format == SurfaceFormat::RGBA32Float);
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(maskPixel[channel] == Catch::Approx(115.0 / 255.0).margin(1e-6));
	CHECK(maskPixel[3] == Catch::Approx(37.0 / 255.0).margin(1e-6));
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
		const NodeRun formatted = PerlinExtraWithValues({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(formatted);
		CHECK(formatted.Output().Format == formats[size_t(depth - 2)]);
	}
}

TEST_CASE("Perlin Extra can publish a typed raster noise field", "[source_perlin_extra][noise_field]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Node{
		"noise",
		"pc.perlin_extra",
		"",
		{},
		{{"dimension", Vector2{3, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"noise_type", EnumValue{2}},
		 {"iteration", int64_t{2}},
		 {"position", Vector2{.17, .31}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{3.25, 2.75}},
		 {"scale_mapped", true},
		 {"parameter_a", .35},
		 {"parameter_a_mapped", true}}
	}};
	document.Outputs = {{"image", "noise", "surface_out"}, {"field", "noise", "field"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "image", image, diagnostic) == Status::Ok);
	EvaluatedValue evaluated;
	REQUIRE(EvaluateValue(document, plan, "field", {}, evaluated, diagnostic) == Status::Ok);
	const auto *field = std::get_if<NoiseFieldValue>(&evaluated.Data);
	REQUIRE(field);
	REQUIRE(field->Data);
	REQUIRE(field->Data->Raster);
	CHECK(field->Data->Raster->Width == image.Width);
	CHECK(field->Data->Raster->Height == image.Height);
	CHECK(field->Data->Raster->Format == image.Format);
	CHECK(field->Data->Raster->Pixels == image.Pixels);
}

TEST_CASE("Perlin Extra mapped controls use map RGB and ignore alpha", "[source_perlin_extra]") {
	const Image map = FloatPixel({1, 0, 0, 0});
	const NodeRun mappedScale = PerlinExtraWithValues({{"scale", Vector2{3, 6}}}, {{"scale_map", &map}});
	const NodeRun scalarScale = PerlinExtraWithValues({{"scale", Vector2{4, 4}}});
	RequireSuccess(mappedScale);
	RequireSuccess(scalarScale);
	SamePixels(mappedScale.Output(), scalarScale.Output());

	const NodeRun mappedA = PerlinExtraWithValues(
		{{"noise_type", EnumValue{3}}, {"parameter_a", Vector2{.2, .8}}}, {{"parameter_a_map", &map}}
	);
	const NodeRun scalarA =
		PerlinExtraWithValues({{"noise_type", EnumValue{3}}, {"parameter_a", Vector2{.4, .4}}});
	RequireSuccess(mappedA);
	RequireSuccess(scalarA);
	SamePixels(mappedA.Output(), scalarA.Output());
}

TEST_CASE("Perlin Extra reports invalid controls and refuses Atlas samplers", "[source_perlin_extra]") {
	const NodeRun equalLevels = PerlinExtraWithValues({{"level_in", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevels.Ok);
	CHECK(equalLevels.Code == Status::UnsupportedExecution);
	CHECK(equalLevels.Port == "level_in");
	const NodeRun badRotation =
		PerlinExtraWithValues({{"rotation", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(badRotation.Ok);
	CHECK(badRotation.Code == Status::InvalidValue);
	CHECK(badRotation.Port == "rotation");
	const NodeRun missingSeed = RunNode(
		"pc.perlin_extra",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"noise_type", EnumValue{0}},
		 {"scale", Vector2{3, 4}},
		 {"scale_mapped", true}}
	);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	const NodeRun unusedParameterA = PerlinExtraWithValues(
		{{"noise_type", EnumValue{2}}, {"iteration", int64_t{1}}, {"parameter_a_mapped", false}}
	);
	RequireSuccess(unusedParameterA);
	const NodeRun unmappedScale =
		PerlinExtraWithValues({{"noise_type", EnumValue{2}}, {"scale_mapped", false}});
	CHECK_FALSE(unmappedScale.Ok);
	CHECK(unmappedScale.Code == Status::UnsupportedExecution);
	CHECK(unmappedScale.Port == "scale");
	const NodeRun unmappedParameterA =
		PerlinExtraWithValues({{"noise_type", EnumValue{3}}, {"parameter_a_mapped", false}});
	CHECK_FALSE(unmappedParameterA.Ok);
	CHECK(unmappedParameterA.Code == Status::UnsupportedExecution);
	CHECK(unmappedParameterA.Port == "parameter_a");
	const NodeRun consumedParameterA =
		PerlinExtraWithValues({{"noise_type", EnumValue{3}}, {"parameter_a", 0.0}});
	CHECK_FALSE(consumedParameterA.Ok);
	CHECK(consumedParameterA.Code == Status::UnsupportedExecution);
	CHECK(consumedParameterA.Port == "parameter_a");

	const AtlasValue atlas = SurfaceAtlas();
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask", "scale_map", "parameter_a_map"}) {
		std::vector<std::pair<std::string_view, Value>> values{{"seed", 17.25}};
		if (port == "scale_map") values.emplace_back("scale_mapped", true);
		if (port == "parameter_a_map") {
			values.emplace_back("noise_type", EnumValue{1});
			values.emplace_back("parameter_a_mapped", true);
			values.emplace_back("parameter_a_map_range", Vector2{0, 0});
		}
		values.emplace_back(port, atlas);
		const NodeRun run = PerlinExtraWithValues(values);
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}
