#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_cellular")
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
	Image BytePixel(std::array<uint8_t, 4> pixel) {
		return Image{1, 1, {pixel[0], pixel[1], pixel[2], pixel[3]}, 0, SurfaceFormat::RGBA8Unorm};
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, 0, pixel));
		return pixel;
	}
	NodeRun Cellular(
		int64_t type = 0,
		int64_t pattern = 0,
		int64_t iteration = 1,
		int64_t blend = 0,
		bool colored = false,
		std::initializer_list<std::pair<std::string_view, Value>> extra = {},
		std::initializer_list<std::pair<std::string_view, const Image *>> images = {}
	) {
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{1, 1}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{.5, .5}},
			{"position_unit", EnumValue{0}},
			{"size", Vector2{1, 1}},
			{"seed", 17.25},
			{"type", EnumValue{type}},
			{"pattern", EnumValue{pattern}},
			{"phase", 0.0},
			{"randomness", 1.0},
			{"rotation", 0.0},
			{"iteration", iteration},
			{"iter_scale", 2.0},
			{"iter_amplitude", .5},
			{"blend_mode", EnumValue{blend}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"contrast", 1.0},
			{"middle", .5},
			{"gap", 0.0},
			{"gap_color", Colour{0, 0, 0, 255}},
			{"colored", colored},
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
		const CatalogueEntry *entry = FindCatalogueEntry("pc.cellular");
		const auto executor = detail::FindExecutor("pc.cellular");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.cellular", "", {}, {}};
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
	void CheckGray(const NodeRun &run, double expected) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output());
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
	void NearPixel(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		const auto a = Pixel(actual);
		const auto b = Pixel(expected);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(a[channel] == Catch::Approx(b[channel]).margin(5e-5));
	}
	void SameImages(const ImageArray &actual, const ImageArray &expected) {
		REQUIRE(actual.Images.size() == expected.Images.size());
		for (size_t index = 0; index < actual.Images.size(); ++index)
			SamePixels(actual.Images[index], expected.Images[index]);
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

TEST_CASE("Cellular types and patterns match independent binary32 references", "[source_cellular]") {
	constexpr std::array<std::array<double, 3>, 4> expected{
		{{{.0002924203872680664, .0002924203872680664, .0147705078125}},
		 {{.38282573223114014, .38282573223114014, -.0072882771492004395}},
		 {{.6875, .6875, .22265625}},
		 {{.3480287790298462, .3480287790298462, .3480287790298462}}}
	};
	for (int64_t type = 0; type < 4; ++type)
		for (int64_t pattern = 0; pattern < 3; ++pattern)
			CheckGray(Cellular(type, pattern), expected[size_t(type)][size_t(pattern)]);
	CheckGray(Cellular(1, 1, 1, 0, false, {{"randomness", .35}, {"rotation", 37.0}}), -.23000705242156982);
	CheckGray(Cellular(0, 0, 2, 0, false, {{"seed", -17.25}}), .13274943828582764);
}

TEST_CASE("Cellular tiled and uniform patterns diverge away from the sample origin", "[source_cellular]") {
	const Image uv = FloatPixel({.25, .25, 0, 1});
	const auto sample = [&](int64_t type, int64_t pattern) {
		return Cellular(
			type,
			pattern,
			1,
			0,
			false,
			{{"uv_mix", 1.0}, {"scale", 3.25}, {"rotation", 37.0}, {"phase", 45.0}},
			{{"uv_map", &uv}}
		);
	};
	CheckGray(sample(0, 0), .24330592155456543);
	CheckGray(sample(0, 1), .48873651027679443);
	// The Edge hash amplifies libm binary32 trig rounding, so keep its references on sinf/cosf.
	CheckGray(sample(1, 0), .06974372267723083);
	CheckGray(sample(1, 1), -.16029804944992065);
	CheckGray(sample(2, 0), .2275390625);
	CheckGray(sample(2, 1), 0.0);
}

TEST_CASE("Cellular crystal ignores pattern and UV maps flip Y", "[source_cellular]") {
	const auto crystal = Cellular(3, 0);
	for (int64_t pattern = 1; pattern < 3; ++pattern)
		SamePixels(Cellular(3, pattern).Output(), crystal.Output());
	const Image uv = FloatPixel({.25, .25, 0, 1});
	CheckGray(Cellular(0, 0, 1, 0, false, {{"uv_mix", 1.0}}, {{"uv_map", &uv}}), .81075119972229);

	const Image halfAlphaUv = FloatPixel({.25, .25, 0, .5});
	const NodeRun unmapped = Cellular();
	const NodeRun zeroMix = Cellular(0, 0, 1, 0, false, {{"uv_mix", 0.0}}, {{"uv_map", &halfAlphaUv}});
	REQUIRE(unmapped.Ok);
	REQUIRE(zeroMix.Ok);
	const auto original = Pixel(unmapped.Output());
	const auto mixed = Pixel(zeroMix.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(mixed[channel] == original[channel]);
	CHECK(mixed[3] == Catch::Approx(original[3] * .5).margin(1e-6));
	const Image crystalUv = FloatPixel({.25, .25, 0, 1});
	CheckGray(
		Cellular(
			3,
			0,
			2,
			0,
			false,
			{{"uv_mix", 1.0}, {"rotation", 37.0}, {"size", Vector2{.8, 1.2}}},
			{{"uv_map", &crystalUv}}
		),
		.5261362791061401
	);
}

TEST_CASE("Cellular fractional raw dimensions retain shader UV denominator", "[source_cellular]") {
	const NodeRun fractional = Cellular(0, 0, 1, 0, false, {{"dimension", Vector2{2.5, 1}}});
	REQUIRE(fractional.Ok);
	CHECK(fractional.Output().Width == 2);
	CHECK(fractional.Output().Height == 1);
	const auto first = Pixel(fractional.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(first[channel] == Catch::Approx(.34985995292663574).margin(5e-5));
}

TEST_CASE("Cellular mapped scale uses mean RGB and ignores map alpha", "[source_cellular]") {
	const Image transparent = BytePixel({255, 0, 0, 0});
	const Image opaque = BytePixel({255, 0, 0, 255});
	const NodeRun mapped = Cellular(
		0,
		1,
		1,
		0,
		false,
		{{"scale_mapped", true}, {"scale_map_range", Vector2{2, 6}}},
		{{"scale_map", &transparent}}
	);
	const NodeRun otherAlpha = Cellular(
		0,
		1,
		1,
		0,
		false,
		{{"scale_mapped", true}, {"scale_map_range", Vector2{2, 6}}},
		{{"scale_map", &opaque}}
	);
	const NodeRun direct = Cellular(0, 1, 1, 0, false, {{"scale", 10. / 3.}});
	REQUIRE(mapped.Ok);
	REQUIRE(otherAlpha.Ok);
	REQUIRE(direct.Ok);
	SamePixels(mapped.Output(), otherAlpha.Output());
	NearPixel(mapped.Output(), direct.Output());
}

TEST_CASE(
	"Cellular colored cells, gap color, and inactive iterations retain source behavior", "[source_cellular]"
) {
	const NodeRun colored = Cellular(2, 0, 2, 0, true);
	REQUIRE(colored.Ok);
	const auto pixel = Pixel(colored.Output());
	CHECK(pixel[0] == Catch::Approx(0).margin(1e-6));
	CHECK(pixel[1] == Catch::Approx(.02170138992369175).margin(5e-5));
	CHECK(pixel[2] == Catch::Approx(.3483073115348816).margin(5e-5));
	const NodeRun gap = Cellular(2, 0, 1, 0, true, {{"gap", 1.0}, {"gap_color", Colour{51, 102, 153, 255}}});
	REQUIRE(gap.Ok);
	const auto gapPixel = Pixel(gap.Output());
	CHECK(gapPixel[0] == Catch::Approx(.2).margin(1e-6));
	CHECK(gapPixel[1] == Catch::Approx(.4).margin(1e-6));
	CHECK(gapPixel[2] == Catch::Approx(.6).margin(1e-6));
	for (int64_t iteration : {0, -1})
		CheckGray(Cellular(0, 0, iteration), 0);
}

TEST_CASE("Cellular honors dimension and position units and supports native depths", "[source_cellular]") {
	auto pixelUnits = Graph("pc.cellular", {4, 3});
	Set(pixelUnits, "dimension_unit", EnumValue{0});
	Set(pixelUnits, "position", Vector2{1, 1.5});
	Set(pixelUnits, "position_unit", EnumValue{0});
	Set(pixelUnits, "size", Vector2{2, 1.5});
	auto referenceUnits = Graph("pc.cellular", {1, 1});
	referenceUnits.Project = ProjectSettings{.SurfaceWidth = 4, .SurfaceHeight = 3};
	Set(referenceUnits, "dimension_unit", EnumValue{1});
	Set(referenceUnits, "position", Vector2{.25, .5});
	Set(referenceUnits, "position_unit", EnumValue{1});
	Set(referenceUnits, "size", Vector2{2, 1.5});
	SamePixels(Draw(pixelUnits), Draw(referenceUnits));
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
		const NodeRun run = Cellular(0, 0, 1, 0, false, {{"attribute_color_depth", EnumValue{depth}}});
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == formats[size_t(depth - 2)]);
		const auto format = DescribeSurfaceFormat(run.Output().Format);
		REQUIRE(format);
		CHECK(run.Output().Pixels.size() == format->BytesPerPixel);
	}
}

TEST_CASE("Cellular projects linked Surface and SurfaceArray position and size", "[source_cellular]") {
	for (const std::string_view port : {"position", "size"}) {
		auto surface = Graph("pc.cellular", {4, 3});
		surface.Nodes.push_back(Solid("numeric", {2, 1}, {255, 255, 255, 255}));
		surface.Links = {{"numeric", "surface_out", "generator", std::string(port)}};
		Set(surface, "position_unit", EnumValue{0});
		auto direct = Graph("pc.cellular", {4, 3});
		Set(direct, std::string(port), Vector2{2, 1});
		Set(direct, "position_unit", EnumValue{0});
		SamePixels(Draw(surface), Draw(direct));

		auto array = Graph("pc.cellular", {4, 3});
		SurfaceRows(array, port);
		Set(array, "position_unit", EnumValue{0});
		auto arrayDirect = Graph("pc.cellular", {4, 3});
		Set(arrayDirect, std::string(port), Vector2{1, 1});
		Set(arrayDirect, "position_unit", EnumValue{0});
		SamePixels(Draw(array), Draw(arrayDirect));
	}
}

TEST_CASE("Cellular linked scalar scale projects two unmapped rows and mapped ranges", "[source_cellular]") {
	auto linked = Graph("pc.cellular", {1, 1});
	Set(linked, "pattern", EnumValue{1});
	linked.Nodes.push_back(Solid("scaleSurface", {2, 1}, {255, 255, 255, 255}));
	linked.Links = {{"scaleSurface", "surface_out", "generator", "scale"}};
	const ImageArray rows = DrawArray(linked);
	REQUIRE(rows.Images.size() == 2);
	for (size_t index = 0; index < 2; ++index) {
		auto direct = Graph("pc.cellular", {1, 1});
		Set(direct, "pattern", EnumValue{1});
		Set(direct, "scale", index == 0 ? 2.0 : 1.0);
		SamePixels(rows.Images[index], Draw(direct));
	}

	auto mapped = Graph("pc.cellular", {1, 1});
	Set(mapped, "pattern", EnumValue{1});
	Set(mapped, "scale_mapped", true);
	Set(mapped, "scale_map_range", Vector2{2, 6});
	mapped.Nodes.push_back(Solid("scaleMap", {1, 1}, {255, 0, 0, 255}));
	mapped.Links = {{"scaleMap", "surface_out", "generator", "scale_map"}};
	auto direct = Graph("pc.cellular", {1, 1});
	Set(direct, "pattern", EnumValue{1});
	Set(direct, "scale", 10. / 3.);
	NearPixel(Draw(mapped), Draw(direct));

	auto surfaceArray = Graph("pc.cellular", {1, 1});
	Set(surfaceArray, "pattern", EnumValue{1});
	SurfaceRows(surfaceArray, "scale");
	const ImageArray scaleRows = DrawArray(surfaceArray);
	REQUIRE(scaleRows.Images.size() == 2);
	auto arrayDirect = Graph("pc.cellular", {1, 1});
	Set(arrayDirect, "pattern", EnumValue{1});
	Set(arrayDirect, "scale", 1.0);
	const Image expectedScale = Draw(arrayDirect);
	for (const Image &row : scaleRows.Images)
		SamePixels(row, expectedScale);
}

TEST_CASE(
	"Cellular Dimension SurfaceArrays preserve varying sizes and collapse equal sizes", "[source_cellular]"
) {
	const auto surfaceDimensions = [] {
		auto document = Graph("pc.cellular", {1, 1});
		SurfaceRows(document, "dimension");
		return DrawArray(document);
	};
	const ImageArray varyingSurfaces = surfaceDimensions();
	auto varyingValuesDocument = Graph("pc.cellular", {1, 1});
	varyingValuesDocument.Nodes.push_back(
		Array("dimensions", ValueType::Vector2, Vector2{2, 1}, Vector2{3, 2})
	);
	varyingValuesDocument.Links = {{"dimensions", "array", "generator", "dimension"}};
	const ImageArray varyingValues = DrawArray(varyingValuesDocument);
	SameImages(varyingSurfaces, varyingValues);

	auto equalSurfaces = Graph("pc.cellular", {1, 1});
	equalSurfaces.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
	equalSurfaces.Nodes.push_back(Solid("second", {2, 1}, {255, 255, 255, 255}));
	Node equalList{"surfaces", "value.array", "", {}, {}};
	equalList.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	equalSurfaces.Nodes.push_back(std::move(equalList));
	equalSurfaces.Links = {
		{"first", "surface_out", "surfaces", "first"},
		{"second", "surface_out", "surfaces", "second"},
		{"surfaces", "array", "generator", "dimension"}
	};
	auto scalarDimension = Graph("pc.cellular", {1, 1});
	Set(scalarDimension, "dimension", Vector2{2, 1});
	SamePixels(Draw(equalSurfaces), Draw(scalarDimension));
}

TEST_CASE("Cellular mask uses RGBA8 scratch and alpha-only is inert", "[source_cellular]") {
	const NodeRun source = Cellular();
	const Image mask = BytePixel({128, 128, 128, 128});
	const NodeRun masked = Cellular(0, 0, 1, 0, false, {}, {{"mask", &mask}});
	REQUIRE(source.Ok);
	REQUIRE(masked.Ok);
	SurfacePixel sourcePixel = Pixel(source.Output());
	Image scratch{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
	sourcePixel[3] *= (128. / 255.) * (128. / 255.);
	REQUIRE(StoreSurfacePixel(scratch, 0, 0, sourcePixel));
	const SurfacePixel expected = Pixel(scratch);
	const auto actual = Pixel(masked.Output());
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(1e-6));
	CHECK(actual[3] == Catch::Approx(expected[3]).margin(1e-6));
	const NodeRun alphaOnly = Cellular(0, 0, 1, 0, false, {{"mask_alpha_only", true}}, {{"mask", &mask}});
	REQUIRE(alphaOnly.Ok);
	SamePixels(alphaOnly.Output(), masked.Output());
}

TEST_CASE("Cellular refuses undefined level spans and consumed amplitudes", "[source_cellular]") {
	const NodeRun level = Cellular(0, 0, 1, 0, false, {{"level_in", Vector2{1, 1}}});
	CHECK_FALSE(level.Ok);
	CHECK(level.Code == Status::UnsupportedExecution);
	CHECK(level.Port == "level_in");
	const NodeRun amplitude = Cellular(0, 0, 2, 0, false, {{"iter_amplitude", 0.0}});
	CHECK_FALSE(amplitude.Ok);
	CHECK(amplitude.Code == Status::UnsupportedExecution);
	CHECK(amplitude.Port == "iter_amplitude");
}

TEST_CASE("Cellular preserves defined radial, size, and empty-iteration seams", "[source_cellular]") {
	const NodeRun zeroRadialAmount = Cellular(0, 2, 1, 0, false, {{"scale", .5}});
	CHECK_FALSE(zeroRadialAmount.Ok);
	CHECK(zeroRadialAmount.Code == Status::UnsupportedExecution);
	CHECK(zeroRadialAmount.Port == "scale");
	for (const int64_t type : {1, 2}) {
		const NodeRun noCandidate = Cellular(type, 2, 1, 0, false, {{"scale", -4.0}});
		CHECK_FALSE(noCandidate.Ok);
		CHECK(noCandidate.Code == Status::UnsupportedExecution);
		CHECK(noCandidate.Port == "scale");
	}
	const NodeRun zeroSize = Cellular(0, 0, 1, 0, false, {{"size", Vector2{0, 1}}});
	CHECK_FALSE(zeroSize.Ok);
	CHECK(zeroSize.Code == Status::UnsupportedExecution);
	CHECK(zeroSize.Port == "size");
	for (const double amplitude : {0.0, 1.0})
		CheckGray(Cellular(0, 0, 0, 0, false, {{"iter_amplitude", amplitude}}), 0);

	const NodeRun missingSeed = RunNode("pc.cellular", {}, {{"dimension", Vector2{1, 1}}});
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
}
