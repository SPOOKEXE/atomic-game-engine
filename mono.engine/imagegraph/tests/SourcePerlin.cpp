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

TEST_SUITE_ID("engine.imagegraph.source_perlin")
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
	NodeRun Perlin(
		std::initializer_list<std::pair<std::string_view, Value>> extra = {},
		std::initializer_list<std::pair<std::string_view, const Image *>> images = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{4, 4}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{0, 0}},
			{"position_unit", EnumValue{0}},
			{"scale", Vector2{3, 2}},
			{"seed", 17.25},
			{"phase", 0.0},
			{"iteration", int64_t{4}},
			{"tile", true},
			{"rotation", 0.0},
			{"blend_method", EnumValue{0}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"color_mode", EnumValue{0}},
			{"color_r_range", Vector2{0, 1}},
			{"color_g_range", Vector2{0, 1}},
			{"color_b_range", Vector2{0, 1}},
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
		const CatalogueEntry *entry = FindCatalogueEntry("pc.perlin");
		const auto executor = detail::FindExecutor("pc.perlin");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.perlin", "", {}, {}};
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
	void SamePixels(const Image &actual, const Image &expected) {
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
	void CheckRGB(const NodeRun &run, const std::array<double, 3> &expected) {
		RequireSuccess(run);
		const SurfacePixel pixel = Pixel(run.Output());
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(pixel[channel] == Catch::Approx(expected[channel]).margin(5e-5));
		CHECK(pixel[3] == Catch::Approx(1).margin(1e-6));
	}
}

TEST_CASE("Perlin tile, blend, and color modes match independent binary32 samples", "[source_perlin]") {
	constexpr std::array<std::array<double, 3>, 12> expected{
		{{{.5, .5, .5}},
		 {{.5, .3712875247001648, .48335734009742737}},
		 {{.3038927912712097, .4833572506904602, .48335734009742737}},
		 {{.5, .5, .5}},
		 {{.5, .41795822978019714, .5327081084251404}},
		 {{.3100583553314209, .5327079892158508, .5327081084251404}},
		 {{.5, .5, .5}},
		 {{.5, .779588520526886, .4695909917354584}},
		 {{.10350324213504791, .46959081292152405, .4695909917354584}},
		 {{.5, .5, .5}},
		 {{.5, .8222602009773254, .4276215136051178}},
		 {{.07600536197423935, .42762133479118347, .4276215136051178}}}
	};
	size_t sample = 0;
	for (const bool tile : {false, true})
		for (int64_t blend = 0; blend < 2; ++blend)
			for (int64_t color = 0; color < 3; ++color) {
				const NodeRun run = Perlin(
					{{"dimension", Vector2{1, 1}},
					 {"position", Vector2{0, 0}},
					 {"scale", Vector2{4, 4}},
					 {"seed", 17.25},
					 {"iteration", int64_t{4}},
					 {"tile", tile},
					 {"blend_method", EnumValue{blend}},
					 {"color_mode", EnumValue{color}}}
				);
				CheckRGB(run, expected[sample++]);
			}
}

TEST_CASE("Perlin off-center rotation, phase, seed, and iteration match references", "[source_perlin]") {
	constexpr std::array<std::array<double, 3>, 12> expected{
		{{{.37553641200065613, .37553641200065613, .37553641200065613}},
		 {{.37553641200065613, .7339307069778442, .6115138530731201}},
		 {{.16270506381988525, .6115138530731201, .27635183930397034}},
		 {{.284236341714859, .284236341714859, .284236341714859}},
		 {{.284236341714859, .7920944690704346, .6655869483947754}},
		 {{.2936851680278778, .6655869483947754, .13837920129299164}},
		 {{.5280806422233582, .5280806422233582, .5280806422233582}},
		 {{.5280806422233582, .5856369733810425, .4034884572029114}},
		 {{.16719070076942444, .3636760711669922, .4034884572029114}},
		 {{.39979976415634155, .39979976415634155, .39979976415634155}},
		 {{.39979976415634155, .5832256078720093, .48370397090911865}},
		 {{.20159542560577393, .48370397090911865, .31409987807273865}}}
	};
	const Image uv = FloatPixel({.3125, .3125, 0, 1});
	size_t sample = 0;
	for (const bool tile : {false, true})
		for (int64_t blend = 0; blend < 2; ++blend)
			for (int64_t color = 0; color < 3; ++color) {
				const NodeRun run = Perlin(
					{{"dimension", Vector2{1, 1}},
					 {"position", Vector2{.1, -.2}},
					 {"position_unit", EnumValue{0}},
					 {"scale", Vector2{3.25, 2.75}},
					 {"seed", -17.25},
					 {"phase", 45.0},
					 {"iteration", int64_t{3}},
					 {"tile", tile},
					 {"rotation", 37.0},
					 {"scaling", 1.75},
					 {"amplitude", .6},
					 {"blend_method", EnumValue{blend}},
					 {"color_mode", EnumValue{color}},
					 {"uv_mix", 1.0}},
					{{"uv_map", &uv}}
				);
				CheckRGB(run, expected[sample++]);
			}
}

TEST_CASE("Perlin levels feed ordered RGB and HSV ranges", "[source_perlin]") {
	constexpr std::array<std::array<double, 3>, 2> expected{
		{{{.6416798233985901, .43787217140197754, .6396864652633667}},
		 {{.35958555340766907, .4015785753726959, .6396864652633667}}}
	};
	const Image uv = FloatPixel({.3125, .3125, 0, 1});
	for (int64_t color = 1; color <= 2; ++color) {
		const NodeRun run = Perlin(
			{{"dimension", Vector2{1, 1}},
			 {"position", Vector2{0, 0}},
			 {"scale", Vector2{3.25, 2.75}},
			 {"tile", false},
			 {"color_mode", EnumValue{color}},
			 {"level_in", Vector2{.2, .8}},
			 {"level_out", Vector2{-.25, 1.25}},
			 {"color_r_range", Vector2{.1, .9}},
			 {"color_g_range", Vector2{.2, .7}},
			 {"color_b_range", Vector2{.3, .8}},
			 {"uv_mix", 1.0}},
			{{"uv_map", &uv}}
		);
		CheckRGB(run, expected[size_t(color - 1)]);
	}
}

TEST_CASE("Perlin tile mode ignores rotation while free mode uses it", "[source_perlin]") {
	const NodeRun tiled = Perlin({{"tile", true}});
	const NodeRun tiledRotated = Perlin({{"tile", true}, {"rotation", 73.0}});
	const NodeRun free = Perlin({{"tile", false}});
	const NodeRun freeRotated = Perlin({{"tile", false}, {"rotation", 73.0}});
	for (const NodeRun *run : {&tiled, &tiledRotated, &free, &freeRotated})
		RequireSuccess(*run);
	SamePixels(tiled.Output(), tiledRotated.Output());
	CHECK(free.Output().Pixels != freeRotated.Output().Pixels);
}

TEST_CASE("Perlin color modes apply channel ranges and HSV conversion", "[source_perlin]") {
	const NodeRun gray = Perlin();
	const NodeRun rgb = Perlin({{"color_mode", EnumValue{1}}});
	const NodeRun rgbFixed = Perlin(
		{{"color_mode", EnumValue{1}},
		 {"color_r_range", Vector2{.2, .2}},
		 {"color_g_range", Vector2{.4, .4}},
		 {"color_b_range", Vector2{.8, .8}}}
	);
	const NodeRun hsvFixed = Perlin(
		{{"color_mode", EnumValue{2}},
		 {"color_r_range", Vector2{.25, .25}},
		 {"color_g_range", Vector2{1, 1}},
		 {"color_b_range", Vector2{.5, .5}}}
	);
	for (const NodeRun *run : {&gray, &rgb, &rgbFixed, &hsvFixed})
		RequireSuccess(*run);
	const auto grayPixel = Pixel(gray.Output());
	CHECK(grayPixel[0] == Catch::Approx(grayPixel[1]).margin(1e-6));
	CHECK(grayPixel[1] == Catch::Approx(grayPixel[2]).margin(1e-6));
	const auto fixed = Pixel(rgbFixed.Output());
	CHECK(fixed[0] == Catch::Approx(.2).margin(1e-6));
	CHECK(fixed[1] == Catch::Approx(.4).margin(1e-6));
	CHECK(fixed[2] == Catch::Approx(.8).margin(1e-6));
	CHECK(Pixel(rgb.Output())[0] != Pixel(rgb.Output())[1]);
	const NodeRun hsvDifferentSeed = Perlin(
		{{"color_mode", EnumValue{2}},
		 {"color_r_range", Vector2{.25, .25}},
		 {"color_g_range", Vector2{1, 1}},
		 {"color_b_range", Vector2{.5, .5}},
		 {"seed", -9.0}}
	);
	RequireSuccess(hsvDifferentSeed);
	SamePixels(hsvFixed.Output(), hsvDifferentSeed.Output());
}

TEST_CASE("Perlin UV mapping flips green, mixes coordinates, and applies sampler alpha", "[source_perlin]") {
	const Image uv = FloatPixel({.8, .2, 0, .5});
	const Image unflippedUv = FloatPixel({.8, .8, 0, .5});
	const NodeRun noMap = Perlin({{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}});
	const NodeRun mapped =
		Perlin({{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}, {"uv_mix", 1.0}}, {{"uv_map", &uv}});
	const NodeRun unflipped = Perlin(
		{{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}, {"uv_mix", 1.0}}, {{"uv_map", &unflippedUv}}
	);
	const NodeRun zeroMix =
		Perlin({{"dimension", Vector2{1, 1}}, {"scale", Vector2{3, 3}}, {"uv_mix", 0.0}}, {{"uv_map", &uv}});
	RequireSuccess(noMap);
	RequireSuccess(mapped);
	RequireSuccess(unflipped);
	RequireSuccess(zeroMix);
	const auto a = Pixel(noMap.Output());
	const auto b = Pixel(mapped.Output());
	const auto c = Pixel(zeroMix.Output());
	CHECK(a[0] != b[0]);
	CHECK(b[0] != Pixel(unflipped.Output())[0]);
	CHECK(c[0] == Catch::Approx(a[0]).margin(1e-6));
	CHECK(c[3] == Catch::Approx(.5).margin(1e-6));
}

TEST_CASE("Perlin mapped controls use RGB average and ignore map alpha", "[source_perlin]") {
	const Image transparent = FloatPixel({1, 0, 0, 0});
	const Image opaque = FloatPixel({1, 0, 0, 1});
	const auto mapped = [&](const Image &image) {
		return Perlin({{"scale", Vector2{1, 3}}, {"scale_mapped", true}}, {{"scale_map", &image}});
	};
	const NodeRun mappedTransparent = mapped(transparent);
	const NodeRun mappedOpaque = mapped(opaque);
	const NodeRun direct = Perlin({{"scale", Vector2{5. / 3., 5. / 3.}}});
	RequireSuccess(mappedTransparent);
	RequireSuccess(mappedOpaque);
	RequireSuccess(direct);
	SamePixels(mappedTransparent.Output(), mappedOpaque.Output());
	SamePixels(mappedTransparent.Output(), direct.Output());
	const Image gray = FloatPixel({.5, .5, .5, 0});
	const NodeRun scalingMapped =
		Perlin({{"scaling", Vector2{1, 3}}, {"scaling_mapped", true}}, {{"scaling_map", &gray}});
	const NodeRun scalingDirect = Perlin({{"scaling", 2.0}});
	RequireSuccess(scalingMapped);
	RequireSuccess(scalingDirect);
	SamePixels(scalingMapped.Output(), scalingDirect.Output());
	const NodeRun amplitudeMapped =
		Perlin({{"amplitude", Vector2{.25, .75}}, {"amplitude_mapped", true}}, {{"amplitude_map", &gray}});
	const NodeRun amplitudeDirect = Perlin({{"amplitude", .5}});
	RequireSuccess(amplitudeMapped);
	RequireSuccess(amplitudeDirect);
	SamePixels(amplitudeMapped.Output(), amplitudeDirect.Output());
}

TEST_CASE("Perlin level mapping and mask preserve output and alpha contracts", "[source_perlin]") {
	const NodeRun identity = Perlin();
	const NodeRun leveled = Perlin({{"level_in", Vector2{0, 1}}, {"level_out", Vector2{.2, .8}}});
	RequireSuccess(identity);
	RequireSuccess(leveled);
	CHECK(Pixel(leveled.Output())[0] == Catch::Approx(.2 + .6 * Pixel(identity.Output())[0]).margin(5e-5));
	const Image mask = FloatPixel({.1, .2, .3, .5});
	const NodeRun masked = Perlin({}, {{"mask", &mask}});
	RequireSuccess(masked);
	const auto before = Pixel(identity.Output());
	const auto after = Pixel(masked.Output());
	Image scratch{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
	SurfacePixel expectedMask = before;
	const double maskMean = (.1 + .2 + .3) / 3. * .5;
	expectedMask[3] *= maskMean;
	REQUIRE(StoreSurfacePixel(scratch, 0, 0, expectedMask));
	const SurfacePixel storedMask = Pixel(scratch);
	for (size_t channel = 0; channel < 4; ++channel)
		CHECK(after[channel] == Catch::Approx(storedMask[channel]).margin(1e-6));
	const NodeRun alphaOnly = Perlin({{"mask_alpha_only", true}}, {{"mask", &mask}});
	RequireSuccess(alphaOnly);
	SamePixels(masked.Output(), alphaOnly.Output());
}

TEST_CASE("Perlin native formats and linked Vector2 projections remain typed", "[source_perlin]") {
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
		const NodeRun run = Perlin({{"attribute_color_depth", EnumValue{depth}}});
		RequireSuccess(run);
		CHECK(run.Output().Format == formats[size_t(depth - 2)]);
	}
	for (const std::string_view port : {"position", "scale"}) {
		auto surface = Graph("pc.perlin", {4, 3});
		surface.Nodes.push_back(Solid("numeric", {2, 1}, {255, 255, 255, 255}));
		surface.Links = {{"numeric", "surface_out", "generator", std::string(port)}};
		Set(surface, "position_unit", EnumValue{0});
		auto direct = Graph("pc.perlin", {4, 3});
		Set(direct, std::string(port), Vector2{2, 1});
		Set(direct, "position_unit", EnumValue{0});
		SamePixels(Draw(surface), Draw(direct));

		auto surfaceArray = Graph("pc.perlin", {4, 3});
		SurfaceRows(surfaceArray, port);
		Set(surfaceArray, "position_unit", EnumValue{0});
		auto arrayDirect = Graph("pc.perlin", {4, 3});
		Set(arrayDirect, std::string(port), Vector2{1, 1});
		Set(arrayDirect, "position_unit", EnumValue{0});
		SamePixels(Draw(surfaceArray), Draw(arrayDirect));
	}
}

TEST_CASE(
	"Perlin dimension SurfaceArrays preserve varying rows and collapse equal sizes", "[source_perlin]"
) {
	auto varyingSurfaces = Graph("pc.perlin", {1, 1});
	SurfaceRows(varyingSurfaces, "dimension");
	Set(varyingSurfaces, "dimension_unit", EnumValue{0});
	auto varyingValues = Graph("pc.perlin", {1, 1});
	varyingValues.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{2, 1}, Vector2{3, 2}));
	varyingValues.Links = {{"dimensions", "array", "generator", "dimension"}};
	Set(varyingValues, "dimension_unit", EnumValue{0});
	const ImageArray surfaceRows = DrawArray(varyingSurfaces);
	const ImageArray valueRows = DrawArray(varyingValues);
	REQUIRE(surfaceRows.Images.size() == 2);
	REQUIRE(valueRows.Images.size() == 2);
	for (size_t index = 0; index < 2; ++index)
		SamePixels(surfaceRows.Images[index], valueRows.Images[index]);

	auto equalSurfaces = Graph("pc.perlin", {1, 1});
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
	auto scalar = Graph("pc.perlin", {1, 1});
	Set(scalar, "dimension", Vector2{2, 1});
	Set(scalar, "dimension_unit", EnumValue{0});
	SamePixels(Draw(equalSurfaces), Draw(scalar));
}

TEST_CASE("Perlin raw map and mask inputs reject Atlas payloads", "[source_perlin]") {
	const AtlasValue atlas = SurfaceAtlas();
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask", "scale_map", "scaling_map", "amplitude_map"}) {
		NodeRun run;
		if (port == "scale_map")
			run = RunNode("pc.perlin", {}, {{port, atlas}, {"scale_mapped", true}, {"seed", 17.25}});
		else if (port == "scaling_map")
			run = RunNode("pc.perlin", {}, {{port, atlas}, {"scaling_mapped", true}, {"seed", 17.25}});
		else if (port == "amplitude_map")
			run = RunNode("pc.perlin", {}, {{port, atlas}, {"amplitude_mapped", true}, {"seed", 17.25}});
		else
			run = RunNode("pc.perlin", {}, {{port, atlas}, {"seed", 17.25}});
		INFO(port << ": " << run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}

TEST_CASE("Perlin preserves empty blend loops and refuses undefined controls", "[source_perlin]") {
	const NodeRun addEmpty = Perlin({{"iteration", int64_t{0}}, {"blend_method", EnumValue{0}}});
	CHECK_FALSE(addEmpty.Ok);
	CHECK(addEmpty.Code == Status::UnsupportedExecution);
	CHECK(addEmpty.Port == "iteration");
	const NodeRun maxEmpty = Perlin({{"iteration", int64_t{0}}, {"blend_method", EnumValue{1}}});
	RequireSuccess(maxEmpty);
	const SurfacePixel maxPixel = Pixel(maxEmpty.Output());
	CHECK(maxPixel[0] == Catch::Approx(0).margin(1e-6));
	CHECK(maxPixel[1] == Catch::Approx(0).margin(1e-6));
	CHECK(maxPixel[2] == Catch::Approx(0).margin(1e-6));
	const NodeRun equalLevels = Perlin({{"level_in", Vector2{.5, .5}}});
	CHECK_FALSE(equalLevels.Ok);
	CHECK(equalLevels.Code == Status::UnsupportedExecution);
	CHECK(equalLevels.Port == "level_in");
	const NodeRun zeroTileScale = Perlin({{"scale", Vector2{0, 0}}});
	CHECK_FALSE(zeroTileScale.Ok);
	CHECK(zeroTileScale.Code == Status::UnsupportedExecution);
	CHECK(zeroTileScale.Port == "scale");
	const NodeRun canceledAmplitude =
		Perlin({{"iteration", int64_t{2}}, {"amplitude", -1.0}, {"blend_method", EnumValue{0}}});
	CHECK_FALSE(canceledAmplitude.Ok);
	CHECK(canceledAmplitude.Code == Status::UnsupportedExecution);
	CHECK(canceledAmplitude.Port == "amplitude");
	ArrayValue nestedScale{ValueType::Scalar, {}};
	nestedScale.Nested = {{1.0, 3.0}, {2.0, 4.0}};
	const NodeRun nestedMappedScale = Perlin({{"scale_mapped", true}, {"scale", nestedScale}});
	CHECK_FALSE(nestedMappedScale.Ok);
	CHECK(nestedMappedScale.Code == Status::UnsupportedExecution);
	CHECK(nestedMappedScale.Port == "scale");
	const std::array<std::pair<std::string_view, Value>, 2> overflows{
		{{"seed", std::numeric_limits<double>::max()}, {"scaling", std::numeric_limits<double>::max()}}
	};
	for (const auto &[port, value] : overflows) {
		const NodeRun overflow = Perlin({{port, value}});
		CHECK_FALSE(overflow.Ok);
		CHECK(overflow.Code == Status::InvalidValue);
		CHECK(overflow.Port == port);
	}
}

TEST_CASE("Perlin source seed must be resolved", "[source_perlin]") {
	const NodeRun missingSeed = RunNode("pc.perlin", {}, {{"dimension", Vector2{1, 1}}});
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
}
