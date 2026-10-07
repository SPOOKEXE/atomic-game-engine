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
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_strand_noise")
using namespace engine::imagegraph;
using namespace complex_generator_test;
namespace {
	Document StrandNoise(Vector2 dimension = {4, 3}) {
		Document document = Graph("pc.noise_strand", dimension);
		Set(document, "attribute_color_depth", EnumValue{5});
		Set(document, "thickness", .125);
		return document;
	}
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
	imagegraph_test::NodeRun GoldenSample(
		double seed,
		int64_t mode = 0,
		int64_t axis = 0,
		std::initializer_list<std::pair<std::string_view, Value>> overrides = {},
		bool useUv = true
	) {
		const Image uvMap = FloatPixel({.2, .4, 0, .5});
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{.1, -.05}},
			{"position_unit", EnumValue{0}},
			{"seed", seed},
			{"density", .75},
			{"slope", .35},
			{"curve", Vector2{.25, 1.25}},
			{"curve_scale", 1.75},
			{"curve_shift", .2},
			{"opacity", Vector2{.2, .9}},
			{"thickness", .125},
			{"mode", EnumValue{mode}},
			{"axis", EnumValue{axis}},
			{"uv_mix", 1.0},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : overrides) {
			auto found = std::find_if(values.begin(), values.end(), [&](const auto &entry) {
				return entry.first == port;
			});
			REQUIRE(found != values.end());
			found->second = value;
		}
		const std::vector<std::pair<std::string_view, const Image *>> images =
			useUv ? std::vector<std::pair<std::string_view, const Image *>>{{"uv_map", &uvMap}}
				  : std::vector<std::pair<std::string_view, const Image *>>{};
		const auto *entry = FindCatalogueEntry("pc.noise_strand");
		const auto executor = detail::FindExecutor("pc.noise_strand");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.noise_strand", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = images;
		for (const auto &input : entry->Inputs) {
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
		imagegraph_test::NodeRun run;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
}

TEST_CASE(
	"Strand Noise defaults with positive thickness produce floating grayscale", "[source_strand_noise]"
) {
	const auto image = Draw(StrandNoise());
	CHECK(image.Width == 4);
	CHECK(image.Height == 3);
	CHECK(image.Format == SurfaceFormat::RGBA32Float);
	CHECK(image.Pixels.size() == 4u * 3u * 16u);
	for (uint32_t y = 0; y < image.Height; ++y)
		for (uint32_t x = 0; x < image.Width; ++x) {
			const auto pixel = Pixel(image, x, y);
			CHECK(pixel[0] == pixel[1]);
			CHECK(pixel[1] == pixel[2]);
			CHECK(pixel[3] == 1.0);
		}
}

TEST_CASE("Strand Noise with zero density has no strand fragments", "[source_strand_noise]") {
	for (const double density : {0.0, -.2}) {
		auto document = StrandNoise();
		Set(document, "density", density);
		const auto image = Draw(document);
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x) {
				const auto pixel = Pixel(image, x, y);
				CHECK(pixel[0] == 0.0);
				CHECK(pixel[1] == 0.0);
				CHECK(pixel[2] == 0.0);
				CHECK(pixel[3] == 1.0);
			}
	}
}

TEST_CASE("Strand Noise modes, axis and seed match independent shader samples", "[source_strand_noise]") {
	constexpr std::array<std::pair<int64_t, double>, 3> modes{
		{{0, .3629286587}, {1, .5549497008}, {2, .7441425323}}
	};
	for (const auto &[mode, expected] : modes) {
		const auto run = GoldenSample(17.25, mode);
		INFO("mode " << mode << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output());
		CHECK(pixel[0] == Catch::Approx(expected).margin(1e-5));
		CHECK(pixel[1] == Catch::Approx(expected).margin(1e-5));
		CHECK(pixel[2] == Catch::Approx(expected).margin(1e-5));
		CHECK(pixel[3] == Catch::Approx(.5).margin(1e-6));
	}
	const auto vertical = GoldenSample(17.25, 0, 1);
	REQUIRE(vertical.Ok);
	CHECK(Pixel(vertical.Output())[0] == Catch::Approx(.1234719157).margin(1e-5));

	const auto negative = GoldenSample(-17.25, 0);
	const auto repeated = GoldenSample(100017.25, 0);
	REQUIRE(negative.Ok);
	REQUIRE(repeated.Ok);
	CHECK(Pixel(negative.Output())[0] == Catch::Approx(.4962744117).margin(1e-5));
	CHECK(Pixel(repeated.Output())[0] == Catch::Approx(.3629286587).margin(1e-5));

	const auto reversed =
		GoldenSample(17.25, 0, 0, {{"curve", Vector2{1.25, .25}}, {"opacity", Vector2{.9, .2}}});
	const auto negativeCurveScale = GoldenSample(17.25, 0, 0, {{"curve_scale", -1.75}});
	const auto negativeCurveShift = GoldenSample(17.25, 0, 0, {{"curve_shift", -.2}});
	REQUIRE(reversed.Ok);
	REQUIRE(negativeCurveScale.Ok);
	REQUIRE(negativeCurveShift.Ok);
	CHECK(Pixel(reversed.Output())[0] == Catch::Approx(.2792143822).margin(1e-5));
	CHECK(Pixel(negativeCurveScale.Output())[0] == Catch::Approx(.3260464370).margin(1e-5));
	CHECK(Pixel(negativeCurveShift.Output())[0] == Catch::Approx(.3109297156).margin(1e-5));

	const auto fractionalCanvas = GoldenSample(
		17.25, 0, 0, {{"dimension", Vector2{3.6, 2.6}}, {"density", .9}, {"thickness", .2}}, false
	);
	REQUIRE(fractionalCanvas.Ok);
	CHECK(fractionalCanvas.Output().Width == 4);
	CHECK(fractionalCanvas.Output().Height == 3);
	CHECK(Pixel(fractionalCanvas.Output(), 1, 1)[0] == Catch::Approx(.2786901891).margin(1e-5));

	const auto fractionalDensity = GoldenSample(17.25, 2, 0, {{"density", .26}}, true);
	REQUIRE(fractionalDensity.Ok);
	CHECK(Pixel(fractionalDensity.Output())[0] == Catch::Approx(.6398101449).margin(1e-5));

	const auto signedLevels =
		GoldenSample(17.25, 0, 0, {{"level_in", Vector2{.8, .2}}, {"level_out", Vector2{1.25, -.25}}});
	REQUIRE(signedLevels.Ok);
	CHECK(Pixel(signedLevels.Output())[0] == Catch::Approx(.1573216915).margin(1e-5));

	const auto uvZeroMix = GoldenSample(17.25, 0, 0, {{"uv_mix", 0.0}});
	REQUIRE(uvZeroMix.Ok);
	CHECK(Pixel(uvZeroMix.Output())[0] == Catch::Approx(.0432920121).margin(1e-5));

	const auto zeroWidthBand = GoldenSample(17.25, 1, 0, {{"thickness", 0.0}});
	REQUIRE(zeroWidthBand.Ok);
	CHECK(Pixel(zeroWidthBand.Output())[0] == 0.0);
	CHECK(Pixel(zeroWidthBand.Output())[3] == Catch::Approx(.5).margin(1e-6));

	const auto emptyLoop = GoldenSample(17.25, 0, 0, {{"density", 0.0}});
	REQUIRE(emptyLoop.Ok);
	CHECK(Pixel(emptyLoop.Output())[0] == 0.0);
	CHECK(Pixel(emptyLoop.Output())[3] == Catch::Approx(.5).margin(1e-6));
}

TEST_CASE("Strand Noise UV map retains sampled alpha at zero UV mix", "[source_strand_noise]") {
	const Image uvMap{1, 1, {64, 191, 128, 128}, 0};
	const auto base = imagegraph_test::RunNode(
		"pc.noise_strand",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const auto mapped = imagegraph_test::RunNode(
		"pc.noise_strand",
		{{"uv_map", &uvMap}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"uv_mix", 0.0},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(base.Message << "; " << mapped.Message);
	REQUIRE(base.Ok);
	REQUIRE(mapped.Ok);
	const auto basePixel = Pixel(base.Output());
	const auto mappedPixel = Pixel(mapped.Output());
	CHECK(mappedPixel[0] == basePixel[0]);
	CHECK(mappedPixel[1] == basePixel[1]);
	CHECK(mappedPixel[2] == basePixel[2]);
	CHECK(mappedPixel[3] == Catch::Approx(128.0 / 255.0).margin(1e-6));
}

TEST_CASE("Strand Noise mask uses the RGBA8 intermediate for floating output", "[source_strand_noise]") {
	const Image mask{1, 1, {255, 255, 255, 255}, 0};
	const auto unmasked = imagegraph_test::RunNode(
		"pc.noise_strand",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_out", Vector2{.123456, .123456}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const auto masked = imagegraph_test::RunNode(
		"pc.noise_strand",
		{{"mask", &mask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"level_out", Vector2{.123456, .123456}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(unmasked.Message << "; " << masked.Message);
	REQUIRE(unmasked.Ok);
	REQUIRE(masked.Ok);
	CHECK(Pixel(unmasked.Output())[0] == Catch::Approx(.123456).margin(1e-7));
	CHECK(Pixel(masked.Output())[0] == Catch::Approx(31.0 / 255).margin(1e-7));
	CHECK(masked.Output().Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE("Strand Noise preserves all source colour depths", "[source_strand_noise]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto document = StrandNoise({2, 2});
		Set(document, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(document);
		const auto format = SourceSurfaceFormat(depth);
		REQUIRE(format);
		CHECK(image.Format == *format);
		CHECK(image.Pixels.size() == 4u * DescribeSurfaceFormat(*format)->BytesPerPixel);
	}
}

TEST_CASE("Strand Noise projects linked position and SurfaceArray dimensions", "[source_strand_noise]") {
	auto linkedPosition = StrandNoise({8, 6});
	linkedPosition.Nodes.push_back(Solid("positionSurface", {2, 1}, {255, 255, 255, 255}));
	linkedPosition.Links = {{"positionSurface", "surface_out", "generator", "position"}};
	Set(linkedPosition, "position_unit", EnumValue{0});
	auto directPosition = StrandNoise({8, 6});
	Set(directPosition, "position", Vector2{2, 1});
	Set(directPosition, "position_unit", EnumValue{0});
	CHECK(Draw(linkedPosition) == Draw(directPosition));

	auto referencePosition = StrandNoise({8, 6});
	Set(referencePosition, "position", Vector2{.025, -.0125});
	Set(referencePosition, "position_unit", EnumValue{1});
	auto pixelPosition = StrandNoise({8, 6});
	Set(pixelPosition, "position", Vector2{.2, -.075});
	Set(pixelPosition, "position_unit", EnumValue{0});
	CHECK(Draw(referencePosition) == Draw(pixelPosition));

	auto wholeSurfaces = StrandNoise({8, 6});
	SurfaceRows(wholeSurfaces, "position");
	auto wholeValue = StrandNoise({8, 6});
	Set(wholeValue, "position", Vector2{1, 1});
	Set(wholeValue, "position_unit", EnumValue{0});
	CHECK(Draw(wholeSurfaces) == Draw(wholeValue));

	auto varyingSurfaces = StrandNoise({1, 1});
	SurfaceRows(varyingSurfaces, "dimension");
	const auto drawArray = [](const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray images;
		const Status status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return images;
	};
	const ImageArray surfaceRows = drawArray(varyingSurfaces);
	auto numericDimensions = StrandNoise({1, 1});
	numericDimensions.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{2, 1}, Vector2{3, 2}));
	numericDimensions.Links = {{"dimensions", "array", "generator", "dimension"}};
	const ImageArray numericRows = drawArray(numericDimensions);
	REQUIRE(surfaceRows.Images.size() == numericRows.Images.size());
	for (size_t index = 0; index < surfaceRows.Images.size(); ++index)
		CHECK(surfaceRows.Images[index] == numericRows.Images[index]);
}

TEST_CASE("Strand Noise reports controls it cannot execute", "[source_strand_noise]") {
	const auto zeroWidthBand = imagegraph_test::RunNode(
		"pc.noise_strand",
		{},
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 17.25},
		 {"mode", EnumValue{1}},
		 {"thickness", 0.0},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(zeroWidthBand.Message);
	REQUIRE(zeroWidthBand.Ok);
	CHECK(Pixel(zeroWidthBand.Output())[0] == 0.0);

	auto missingSeed = StrandNoise();
	std::erase_if(missingSeed.Nodes.front().Values, [](const auto &value) { return value.Port == "seed"; });
	Refuse(std::move(missingSeed), Status::UnsupportedExecution, "seed");

	auto zeroInput = StrandNoise();
	Set(zeroInput, "level_in", Vector2{.5, .5});
	Refuse(std::move(zeroInput), Status::UnsupportedExecution, "level_in");

	for (const int64_t mode : {int64_t{0}, int64_t{2}}) {
		auto missingThickness = StrandNoise();
		Set(missingThickness, "mode", EnumValue{mode});
		Set(missingThickness, "thickness", 0.0);
		Refuse(std::move(missingThickness), Status::UnsupportedExecution, "thickness");
	}
}

TEST_CASE("Strand Noise skips absent fragments and empty loops without a seed", "[source_strand_noise]") {
	for (const Vector2 dimension : {Vector2{.2, .2}, Vector2{8, 6}}) {
		auto document = StrandNoise(dimension);
		Set(document, "density", 0.0);
		Set(document, "thickness", 0.0);
		std::erase_if(document.Nodes[0].Values, [](const auto &v) { return v.Port == "seed"; });
		const Image image = Draw(document);
		const auto pixel = Pixel(image);
		CHECK(pixel[0] == 0);
		CHECK(pixel[3] == (dimension.X < .5 ? 0 : 1));
	}
	const auto negative = imagegraph_test::RunNode(
		"pc.noise_strand",
		{},
		{{"dimension", Vector2{8, 6}},
		 {"dimension_unit", EnumValue{0}},
		 {"density", -.5},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(negative.Ok);
	CHECK(Pixel(negative.Output())[0] == 0);
}

TEST_CASE("Strand Noise refuses count overflow and raw Atlas bindings", "[source_strand_noise]") {
	const auto overflow = imagegraph_test::RunNode(
		"pc.noise_strand",
		{},
		{{"dimension", Vector2{8, 6}}, {"dimension_unit", EnumValue{0}}, {"density", 0x1p30}}
	);
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Code == Status::UnsupportedExecution);
	CHECK(overflow.Port == "density");
	const auto nonfinite = GoldenSample(17.25, 0, 0, {{"curve_scale", std::numeric_limits<double>::max()}});
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Code == Status::InvalidValue);
	CHECK(nonfinite.Port == "curve_scale");
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = {1, 1, {64, 128, 192, 255}, 0};
	data.Scale = {1, 1};
	data.Dimension = {1, 1};
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask"}) {
		const auto run = imagegraph_test::RunNode("pc.noise_strand", {}, {{port, atlas}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
}
