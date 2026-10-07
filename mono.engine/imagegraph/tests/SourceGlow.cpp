#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_glow")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image FloatImage(uint32_t width, uint32_t height, SurfacePixel colour) {
		Image image{
			width, height, std::vector<uint8_t>(size_t(width) * height * 16), 0, SurfaceFormat::RGBA32Float
		};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, colour));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x, uint32_t y) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	Image OuterGlowSource() {
		Image image = FloatImage(3, 3, {0, 0, 0, .375});
		REQUIRE(StoreSurfacePixel(image, 0, 1, {.8, .9, 1, .625}));
		return image;
	}
	Curve ConstantCurve(double value) {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 0, 1};
		curve.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return curve;
	}
	Document GlowGraph(Value strength = 1.) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"glow", "pc.glow", "", {}, {{"strength", std::move(strength)}}}
		};
		document.Links = {{"source", "image", "glow", "surface_in"}};
		document.Outputs = {{"out", "glow", "surface_out"}};
		return document;
	}
}

TEST_CASE("Glow uses source radius falloff and preserves greyscale source alpha", "[source_2d][glow]") {
	const Image source = OuterGlowSource();
	const auto run = RunNode(
		"pc.glow",
		{{"surface_in", &source}},
		{{"mode", EnumValue{0}}, {"side", EnumValue{0}}, {"color", Colour{255, 255, 255, 255}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA32Float);
	const auto centre = Pixel(run.Output(), 1, 1);
	CHECK(centre[0] == Catch::Approx(2. / 3).margin(1e-6));
	CHECK(centre[1] == Catch::Approx(2. / 3).margin(1e-6));
	CHECK(centre[2] == Catch::Approx(2. / 3).margin(1e-6));
	CHECK(centre[3] == Catch::Approx(.375).margin(1e-6));
	CHECK(Pixel(run.Output(), 0, 0)[0] == Catch::Approx(2. / 3).margin(1e-6));
}

TEST_CASE("Glow alpha mode creates outer falloff and inner alpha from hit colour", "[source_2d][glow]") {
	Image outer = FloatImage(3, 3, {0, 0, 0, 0});
	REQUIRE(StoreSurfacePixel(outer, 0, 1, {.2, .4, .6, 1}));
	auto outerRun = RunNode(
		"pc.glow",
		{{"surface_in", &outer}},
		{{"mode", EnumValue{1}}, {"side", EnumValue{0}}, {"color", Colour{255, 128, 0, 255}}}
	);
	INFO(outerRun.Message);
	REQUIRE(outerRun.Ok);
	const auto outerCentre = Pixel(outerRun.Output(), 1, 1);
	CHECK(outerCentre[0] == Catch::Approx(1).margin(1e-6));
	CHECK(outerCentre[1] == Catch::Approx(128. / 255).margin(1e-6));
	CHECK(outerCentre[2] == Catch::Approx(0).margin(1e-6));
	CHECK(outerCentre[3] == Catch::Approx(2. / 3).margin(1e-6));

	Image inner = FloatImage(3, 3, {1, 1, 1, 1});
	REQUIRE(StoreSurfacePixel(inner, 0, 1, {0, 0, 0, 0}));
	auto innerRun = RunNode(
		"pc.glow",
		{{"surface_in", &inner}},
		{{"mode", EnumValue{1}}, {"side", EnumValue{1}}, {"color", Colour{64, 128, 192, 255}}}
	);
	INFO(innerRun.Message);
	REQUIRE(innerRun.Ok);
	const auto innerCentre = Pixel(innerRun.Output(), 1, 1);
	CHECK(innerCentre[0] == Catch::Approx(1 + 64. / 255 * 2. / 3).margin(1e-6));
	CHECK(innerCentre[1] == Catch::Approx(1 + 128. / 255 * 2. / 3).margin(1e-6));
	CHECK(innerCentre[2] == Catch::Approx(1 + 192. / 255 * 2. / 3).margin(1e-6));
	CHECK(innerCentre[3] == Catch::Approx(1 + 2. / 3).margin(1e-6));
}

TEST_CASE("Glow no-hit pixels use mode-specific background when original is hidden", "[source_2d][glow]") {
	const Image source = FloatImage(1, 1, {.25, .5, .75, .375});
	for (int64_t mode : {0, 1}) {
		const auto run = RunNode(
			"pc.glow", {{"surface_in", &source}}, {{"mode", EnumValue{mode}}, {"draw_original", false}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output(), 0, 0);
		CHECK(pixel[0] == 0);
		CHECK(pixel[1] == 0);
		CHECK(pixel[2] == 0);
		CHECK(pixel[3] == (mode == 0 ? 1 : 0));
	}
}

TEST_CASE("Glow retains each authored physical blend slot", "[source_2d][glow]") {
	const Image source = OuterGlowSource();
	const std::array<int64_t, 6> modes{0, 1, 3, 4, 6, 7};
	for (size_t index = 0; index < modes.size(); ++index) {
		const auto run = RunNode(
			"pc.glow",
			{{"surface_in", &source}},
			{{"blend_mode", EnumValue{modes[index]}},
			 {"color", Colour{255, 255, 255, 255}},
			 {"mode", EnumValue{0}}}
		);
		INFO("physical blend slot " << modes[index] << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto pixel = Pixel(run.Output(), 1, 1);
		const double strength = 2. / 3;
		const double expected = modes[index] == 6 ? -strength * strength : modes[index] == 7 ? 0 : strength;
		CHECK(pixel[0] == Catch::Approx(expected).margin(2e-6));
		CHECK(pixel[1] == Catch::Approx(expected).margin(2e-6));
		CHECK(pixel[2] == Catch::Approx(expected).margin(2e-6));
		CHECK(pixel[3] == Catch::Approx(.375).margin(1e-6));
	}
}

TEST_CASE("Glow ignores border and applies strength maps and falloff curves", "[source_2d][glow]") {
	const Image source = OuterGlowSource();
	const auto base = RunNode("pc.glow", {{"surface_in", &source}}, {{"border", 4.}});
	const auto defaultBorder = RunNode("pc.glow", {{"surface_in", &source}});
	REQUIRE(base.Ok);
	REQUIRE(defaultBorder.Ok);
	CHECK(base.Output().Pixels == defaultBorder.Output().Pixels);

	const Image blackMap = FloatImage(1, 1, {0, 0, 0, 1});
	const Image whiteMap = FloatImage(1, 1, {1, 1, 1, 1});
	const auto smallRadius = RunNode(
		"pc.glow",
		{{"surface_in", &source}, {"size_map", &blackMap}},
		{{"size_mapped", true}, {"size_map_range", Vector2{1, 3}}}
	);
	const auto largeRadius = RunNode(
		"pc.glow",
		{{"surface_in", &source}, {"size_map", &whiteMap}},
		{{"size_mapped", true}, {"size_map_range", Vector2{1, 3}}}
	);
	REQUIRE(smallRadius.Ok);
	REQUIRE(largeRadius.Ok);
	CHECK(Pixel(smallRadius.Output(), 1, 1)[0] == Catch::Approx(0).margin(1e-6));
	CHECK(Pixel(largeRadius.Output(), 1, 1)[0] == Catch::Approx(2. / 3).margin(1e-6));

	const auto mapped = RunNode(
		"pc.glow",
		{{"surface_in", &source}, {"strength_map", &blackMap}},
		{{"strength_mapped", true}, {"strength_map_range", Vector2{0, 1}}}
	);
	INFO(mapped.Message);
	REQUIRE(mapped.Ok);
	CHECK(Pixel(mapped.Output(), 1, 1) == Pixel(source, 1, 1));

	const auto curved = RunNode(
		"pc.glow",
		{{"surface_in", &source}},
		{{"strength_curved", true}, {"strength_curve", ConstantCurve(.5)}}
	);
	INFO(curved.Message);
	REQUIRE(curved.Ok);
	CHECK(Pixel(curved.Output(), 1, 1)[0] == Catch::Approx(.5).margin(1e-6));
}

TEST_CASE("Glow inactive copy and processor mask mix retain source behavior", "[source_2d][glow]") {
	const Image source = OuterGlowSource();
	const auto inactive = RunNode(
		"pc.glow", {{"surface_in", &source}}, {{"active", false}, {"size", -100.}, {"mode", EnumValue{99}}}
	);
	INFO(inactive.Message);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == source.Pixels);
	CHECK(inactive.Output().Format == source.Format);

	const Image mask = FloatImage(1, 1, {.5, .5, .5, 1});
	const auto mixed = RunNode("pc.glow", {{"surface_in", &source}, {"mask", &mask}}, {{"mix", .5}});
	INFO(mixed.Message);
	REQUIRE(mixed.Ok);
	CHECK(Pixel(mixed.Output(), 1, 1)[0] == Catch::Approx(1. / 6).margin(1e-6));
	CHECK(Pixel(mixed.Output(), 1, 1)[3] == Catch::Approx(.375).margin(1e-6));
}

TEST_CASE("Glow array rows and strength animation execute through graph evaluation", "[source_2d][glow]") {
	const Image source = OuterGlowSource();
	ArrayValue strengths;
	strengths.ElementType = ValueType::Scalar;
	strengths.Elements = {0., 1.};
	auto arrayDocument = GlowGraph(strengths);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(arrayDocument, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto arrayStatus = EvaluateArray(arrayDocument, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(arrayStatus == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(Pixel(output.Images[0], 1, 1)[0] == Catch::Approx(0).margin(1e-6));
	CHECK(Pixel(output.Images[1], 1, 1)[0] == Catch::Approx(2. / 3).margin(1e-6));

	auto animated = GlowGraph(0.);
	animated.Nodes[1].SourceAnimatedInputs = {"strength"};
	animated.Keyframes = {{"glow", "strength", 0, 0.}, {"glow", "strength", 1, 1.}};
	Document restored;
	REQUIRE(Read(Write(animated), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == animated);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	request.Tick = 0;
	Image first;
	REQUIRE(Evaluate(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 1;
	Image later;
	REQUIRE(Evaluate(restored, plan, "out", request, later, diagnostic) == Status::Ok);
	CHECK(Pixel(first, 1, 1)[0] == Catch::Approx(0).margin(1e-6));
	CHECK(Pixel(later, 1, 1)[0] == Catch::Approx(2. / 3).margin(1e-6));
	auto linked = GlowGraph(0.);
	linked.Junctions = {{"strength", "", ValueType::Scalar, 1.}};
	linked.Links.push_back({"strength", "value", "glow", "strength"});
	REQUIRE(Compile(linked, plan, diagnostic) == Status::Ok);
	Image linkedOutput;
	const auto linkedStatus = Evaluate(linked, plan, "out", request, linkedOutput, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(linkedStatus == Status::Ok);
	CHECK(Pixel(linkedOutput, 1, 1)[0] == Catch::Approx(2. / 3).margin(1e-6));
}

TEST_CASE("Glow rejects oversized complete array work before producing a row", "[source_2d][glow]") {
	ArrayValue sizes;
	sizes.ElementType = ValueType::Scalar;
	sizes.Elements = {3., 100000.};
	auto document = GlowGraph(1.);
	document.Nodes[1].Values.push_back({"size", std::move(sizes)});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", OuterGlowSource()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(document, plan, "out", request, output, diagnostic);
	CHECK(status == Status::LimitExceeded);
	CHECK(diagnostic.Message.find("complete batch") != std::string::npos);
	CHECK(output.Images.empty());
}

TEST_CASE("Glow preserves HDR floating and normalized surface formats", "[source_2d][glow]") {
	Image hdr{1, 1, std::vector<uint8_t>(8), 0, SurfaceFormat::RGBA16Float};
	REQUIRE(StoreSurfacePixel(hdr, 0, 0, {2, .5, .25, 1}));
	auto floating = RunNode("pc.glow", {{"surface_in", &hdr}}, {{"active", false}});
	REQUIRE(floating.Ok);
	CHECK(floating.Output().Format == SurfaceFormat::RGBA16Float);
	CHECK(Pixel(floating.Output(), 0, 0)[0] == Catch::Approx(2).margin(1e-3));

	const Image normalized{1, 1, {128, 64, 32, 255}, 0, SurfaceFormat::RGBA8Unorm};
	auto unorm = RunNode("pc.glow", {{"surface_in", &normalized}}, {{"active", false}});
	REQUIRE(unorm.Ok);
	CHECK(unorm.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(unorm.Output().Pixels == normalized.Pixels);
}

TEST_CASE("Glow diagonal hits distinguish ring and floored pixel distances", "[source_2d][glow]") {
	Image source = FloatImage(3, 3, {0, 0, 0, .375});
	REQUIRE(StoreSurfacePixel(source, 0, 0, {1, 1, 1, 1}));
	const auto ring = RunNode("pc.glow", {{"surface_in", &source}}, {{"pixel_distance", true}});
	const auto pixel = RunNode("pc.glow", {{"surface_in", &source}}, {{"pixel_distance", false}});
	REQUIRE(ring.Ok);
	REQUIRE(pixel.Ok);
	CHECK(Pixel(ring.Output(), 1, 1)[0] == Catch::Approx(2. / 3).margin(1e-6));
	CHECK(Pixel(pixel.Output(), 1, 1)[0] == Catch::Approx(1 - std::sqrt(2.) / 3).margin(1e-6));
}

TEST_CASE("Glow texture and winning sample modulate colour before falloff", "[source_2d][glow]") {
	const auto source = OuterGlowSource();
	const auto texture = FloatImage(1, 1, {.25, .5, .75, .5});
	const auto run =
		RunNode("pc.glow", {{"surface_in", &source}, {"texture", &texture}}, {{"blend_color", .5}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto centre = Pixel(run.Output(), 1, 1);
	const SurfacePixel texturePixel{.25, .5, .75, .5};
	const auto hit = Pixel(source, 0, 1);
	for (size_t channel = 0; channel < 3; ++channel)
		CHECK(
			centre[channel] ==
			Catch::Approx(texturePixel[channel] * (1 + hit[channel]) * .5 * 2. / 3).margin(1e-6)
		);
	CHECK(centre[3] == Catch::Approx(.375).margin(1e-6));
}

TEST_CASE("Glow Replace clamps strength while active float output retains HDR", "[source_2d][glow]") {
	const auto source = OuterGlowSource();
	const auto replace =
		RunNode("pc.glow", {{"surface_in", &source}}, {{"blend_mode", EnumValue{1}}, {"strength", 4.}});
	const auto hdr = RunNode("pc.glow", {{"surface_in", &source}}, {{"strength", 4.}});
	const auto normalized = RunNode(
		"pc.glow", {{"surface_in", &source}}, {{"strength", 4.}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(replace.Ok);
	REQUIRE(hdr.Ok);
	INFO(normalized.Message);
	REQUIRE(normalized.Ok);
	CHECK(Pixel(replace.Output(), 1, 1)[0] == 1);
	CHECK(hdr.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(Pixel(hdr.Output(), 1, 1)[0] == Catch::Approx(8. / 3).margin(1e-6));
	CHECK(normalized.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(Pixel(normalized.Output(), 1, 1)[0] == 1);
}

TEST_CASE("Glow shader float conversion selects strict step curve mode", "[source_2d][glow]") {
	const auto source = OuterGlowSource();
	auto curve = ConstantCurve(.25);
	curve.Header[2] = 1 + 1e-8;
	curve.Anchors.back()[3] = .875;
	const auto run =
		RunNode("pc.glow", {{"surface_in", &source}}, {{"strength_curved", true}, {"strength_curve", curve}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 1, 1)[0] == Catch::Approx(.25).margin(1e-6));
}

TEST_CASE("Glow greyscale early return uses RGB rather than alpha weighted brightness", "[source_2d][glow]") {
	Image source = FloatImage(3, 3, {0, 0, 0, 1});
	REQUIRE(StoreSurfacePixel(source, 1, 1, {1, 1, 1, 0}));
	REQUIRE(StoreSurfacePixel(source, 0, 1, {.9, .9, .9, 1}));
	const auto run = RunNode("pc.glow", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 1, 1) == Pixel(source, 1, 1));
}

TEST_CASE("Glow unwraps main SurfaceAtlas and rejects raw auxiliary Atlas", "[source_2d][glow]") {
	const auto source = OuterGlowSource();
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = source;
	data.Dimension = {1, 1};
	const auto main = RunNode("pc.glow", {}, {{"surface_in", atlas}});
	const auto plain = RunNode("pc.glow", {{"surface_in", &source}});
	INFO(main.Message);
	REQUIRE(main.Ok);
	REQUIRE(plain.Ok);
	CHECK(main.Output().Pixels == plain.Output().Pixels);
	const auto raw = RunNode("pc.glow", {{"surface_in", &source}}, {{"texture", atlas}});
	CHECK_FALSE(raw.Ok);
	CHECK(raw.Code == Status::UnsupportedExecution);
	CHECK(raw.Port == "texture");
}

TEST_CASE("Glow reports unsupported and nonfinite controls at their ports", "[source_2d][glow]") {
	const auto source = OuterGlowSource();
	for (int64_t slot : {2, 5}) {
		INFO("blend slot " << slot);
		const auto run = RunNode("pc.glow", {{"surface_in", &source}}, {{"blend_mode", EnumValue{slot}}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "blend_mode");
	}
	const auto clamped = RunNode("pc.glow", {{"surface_in", &source}}, {{"blend_mode", EnumValue{8}}});
	const auto multiply = RunNode("pc.glow", {{"surface_in", &source}}, {{"blend_mode", EnumValue{7}}});
	REQUIRE(clamped.Ok);
	REQUIRE(multiply.Ok);
	CHECK(clamped.Output().Pixels == multiply.Output().Pixels);
	auto invalidArray = GlowGraph();
	ArrayValue modes;
	modes.ElementType = ValueType::Enum;
	modes.Elements = {EnumValue{8}};
	invalidArray.Junctions = {{"modes", "", ValueType::Array, modes}};
	invalidArray.Links.push_back({"modes", "value", "glow", "blend_mode"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compileStatus = Compile(invalidArray, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(compileStatus == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	CHECK(
		EvaluateArray(invalidArray, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "glow");
	CHECK(diagnostic.Port == "blend_mode");
	CHECK(output.Images.empty());
	const auto nonfinite =
		RunNode("pc.glow", {{"surface_in", &source}}, {{"strength", std::numeric_limits<double>::max()}});
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Code == Status::InvalidValue);
	CHECK(nonfinite.Port == "strength");
	auto curve = ConstantCurve(.5);
	curve.Anchors.resize(10);
	const auto longCurve =
		RunNode("pc.glow", {{"surface_in", &source}}, {{"strength_curved", true}, {"strength_curve", curve}});
	CHECK_FALSE(longCurve.Ok);
	CHECK(longCurve.Code == Status::UnsupportedExecution);
	CHECK(longCurve.Port == "strength_curve");
	const auto mask = FloatImage(1, 1, {1, 1, 1, 1});
	const auto feather =
		RunNode("pc.glow", {{"surface_in", &source}, {"mask", &mask}}, {{"mask_feather", 1e10}});
	CHECK_FALSE(feather.Ok);
	CHECK(feather.Code == Status::LimitExceeded);
	CHECK(feather.Port == "mask_feather");
}

TEST_CASE("Glow cumulative admission refuses individually valid rows before execution", "[source_2d][glow]") {
	const auto source = FloatImage(1, 1, {0, 0, 0, 1});
	const auto single = RunNode("pc.glow", {{"surface_in", &source}}, {{"size", 800.}});
	INFO(single.Message);
	REQUIRE(single.Ok);
	const auto *entry = FindCatalogueEntry("pc.glow");
	REQUIRE(entry);
	Node node{"glow", "pc.glow", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	for (const auto &input : entry->Inputs)
		if (auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, std::move(*value));
	ArrayValue sizes;
	sizes.ElementType = ValueType::Scalar;
	sizes.Elements = {800., 800.};
	for (auto &[port, value] : context.Values)
		if (port == "size") value = sizes;
	context.InputProvenanceResolved = true;
	size_t observed = 0;
	const auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	const auto execute = detail::FindExecutor("pc.glow");
	REQUIRE(execute);
	CHECK_FALSE(detail::RunProcessorBatch(context, execute, observer, &observed));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete batch") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
}
