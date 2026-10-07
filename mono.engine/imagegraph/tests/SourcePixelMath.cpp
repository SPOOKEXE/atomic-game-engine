#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_pixel_math")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image FloatImage(SurfacePixel colour, uint32_t width = 1, uint32_t height = 1) {
		Image image{
			width, height, std::vector<uint8_t>(size_t(width) * height * 16), 0, SurfaceFormat::RGBA32Float
		};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, colour));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel colour{};
		REQUIRE(LoadSurfacePixel(image, x, y, colour));
		return colour;
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"math",
			 "pc.pixel_math",
			 "",
			 {},
			 {{"operator", EnumValue{0}}, {"value", Vector4{.25, .25, .25, .25}}}}
		};
		document.Links = {{"source", "image", "math", "surface_in"}};
		document.Outputs = {{"out", "math", "surface_out"}};
		return document;
	}
}
TEST_CASE(
	"Pixel Math preserves all 26 pinned shader slots including menu mismatches", "[source_2d][pixel_math]"
) {
	const auto source = FloatImage({.25, .25, .25, .25});
	constexpr std::array<double, 26> expected{.75,		   -.25,		.125, .5,  .5, .0625, .2474039593,
											  .9689124217, .2553419212, .25,  0,   1,  0,	  .375,
											  .25,		   .25,			0,	  .25, 1,  1,	  0,
											  0,		   .25,			.25,  .25, .25};
	for (int64_t operation = 0; operation < 26; ++operation) {
		auto run = RunNode(
			"pc.pixel_math",
			{{"surface_in", &source}},
			{{"operator", EnumValue{operation}},
			 {"value", Vector4{.5, .5, .5, .5}},
			 {"range", Vector2{.125, .375}},
			 {"mix_2", .5}}
		);
		INFO(operation << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto colour = Pixel(run.Output());
		for (size_t c = 0; c < 4; ++c)
			CHECK(
				colour[c] ==
				Catch::Approx(c == 3 && operation >= 18 && operation <= 21 ? 1 : expected[size_t(operation)])
					.margin(1e-6)
			);
	}
}
TEST_CASE("Pixel Math Round uses shader floor plus half on signed HDR pixels", "[source_2d][pixel_math]") {
	const auto source = FloatImage({-.5, .5, 2.5, -1.5});
	auto run = RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"operator", EnumValue{12}}});
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output()) == SurfacePixel{0, 1, 3, -1});
}
TEST_CASE(
	"Pixel Math Surface operand samples nearest at normalized output pixel centers", "[source_2d][pixel_math]"
) {
	const auto source = FloatImage({.25, .25, .25, .25}, 4, 1);
	auto operand = FloatImage({.5, .25, .125, .75}, 2, 1);
	REQUIRE(StoreSurfacePixel(operand, 1, 0, {.125, .5, .75, .25}));
	auto run = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}, {"operand_surface", &operand}},
		{{"operator", EnumValue{2}}, {"operand_type", EnumValue{1}}}
	);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 0) == SurfacePixel{.125, .0625, .03125, .1875});
	CHECK(Pixel(run.Output(), 1) == Pixel(run.Output(), 0));
	CHECK(Pixel(run.Output(), 2) == SurfacePixel{.03125, .125, .1875, .0625});
	CHECK(Pixel(run.Output(), 3) == Pixel(run.Output(), 2));
}
TEST_CASE(
	"Pixel Math Color operand and Surface clamp retain source operand precedence", "[source_2d][pixel_math]"
) {
	const auto source = FloatImage({.25, .5, .75, .125});
	auto coloured = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}},
		{{"operator", EnumValue{2}}, {"operand_type", EnumValue{2}}, {"color", Colour{255, 128, 0, 255}}}
	);
	REQUIRE(coloured.Ok);
	const auto colour = Pixel(coloured.Output());
	CHECK(colour[0] == .25);
	CHECK(colour[1] == Catch::Approx(.5 * 128. / 255).margin(1e-6));
	CHECK(colour[2] == 0);
	CHECK(colour[3] == .125);
	const auto operand = FloatImage({.375, .625, 0, 0});
	auto clamped = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}, {"operand_surface", &operand}},
		{{"operator", EnumValue{15}}, {"operand_type", EnumValue{1}}, {"range", Vector2{0, 1}}}
	);
	REQUIRE(clamped.Ok);
	CHECK(Pixel(clamped.Output()) == SurfacePixel{.375, .5, .625, .375});
}
TEST_CASE(
	"Pixel Math honors channel mask and outer Mix independently of Lerp Mix", "[source_2d][pixel_math]"
) {
	const auto source = FloatImage({.25, .25, .25, .5});
	const auto mask = FloatImage({1, 1, 1, .5});
	auto run = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}, {"mask", &mask}},
		{{"operator", EnumValue{13}},
		 {"value", Vector4{.75, .75, .75, 1}},
		 {"mix_2", .5},
		 {"mix", .5},
		 {"channel", int64_t{1}}}
	);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output()) == SurfacePixel{.3125, .25, .25, .5});
	auto inverted = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}, {"mask", &mask}},
		{{"operator", EnumValue{0}}, {"value", Vector4{.25, .25, .25, .25}}, {"invert_mask", true}}
	);
	REQUIRE(inverted.Ok);
	CHECK(Pixel(inverted.Output()) == SurfacePixel{.375, .375, .375, .625});
}
TEST_CASE(
	"Pixel Math supports every surface format and explicit depth conversion", "[source_2d][pixel_math]"
) {
	for (auto format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(1, 1, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image source{1, 1, std::vector<uint8_t>(size_t(layout->Bytes)), 0, format};
		REQUIRE(StoreSurfacePixel(source, 0, 0, {.25, .5, .75, 1}));
		auto run = RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"operator", EnumValue{22}}});
		INFO(unsigned(format) << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == format);
		CHECK(run.Output().Pixels == source.Pixels);
	}
	const auto source = FloatImage({.25, .5, .75, .5});
	auto converted =
		RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{3}}});
	REQUIRE(converted.Ok);
	CHECK(converted.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(converted.Output().Pixels == std::vector<uint8_t>{64, 128, 191, 128});
}
TEST_CASE("Pixel Math diagnoses undefined shader math and malformed surfaces", "[source_2d][pixel_math]") {
	const auto source = FloatImage({.25, .25, .25, .25});
	for (int64_t operation : {3, 5, 9, 16}) {
		auto run = RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"operator", EnumValue{operation}}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Port == "operator");
	}
	const auto negative = FloatImage({-.25, .25, .25, .25});
	auto power = RunNode(
		"pc.pixel_math",
		{{"surface_in", &negative}},
		{{"operator", EnumValue{4}}, {"value", Vector4{2, 2, 2, 2}}}
	);
	CHECK_FALSE(power.Ok);
	CHECK(power.Port == "operator");
	auto bounds = RunNode(
		"pc.pixel_math", {{"surface_in", &source}}, {{"operator", EnumValue{15}}, {"range", Vector2{1, 0}}}
	);
	CHECK_FALSE(bounds.Ok);
	CHECK(bounds.Port == "operator");
	Image malformed{1, 1, {1}, 0};
	auto invalid = RunNode("pc.pixel_math", {{"surface_in", &malformed}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Port == "surface_in");
	auto missing = RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"operand_type", EnumValue{1}}});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Port == "operand_surface");
	auto fraction = RunNode("pc.pixel_math", {{"surface_in", &source}}, {{"operator", .5}});
	CHECK_FALSE(fraction.Ok);
	CHECK(fraction.Port == "operator");
}
TEST_CASE("Pixel Math inactive copies ignore active-only controls", "[source_2d][pixel_math]") {
	const auto source = FloatImage({-.5, .5, 2.5, 1});
	auto run = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}},
		{{"active", false},
		 {"operator", EnumValue{999}},
		 {"operand_type", EnumValue{1}},
		 {"value", Vector4{std::numeric_limits<double>::infinity(), 0, 0, 0}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == source.Pixels);
	CHECK(run.Output().Format == source.Format);
}
TEST_CASE("Pixel Math animation and instance links survive document round trip", "[source_2d][pixel_math]") {
	auto document = Graph();
	document.Nodes[1].SourceAnimatedInputs = {"value"};
	document.Keyframes = {
		{"math", "value", 0, Vector4{0, 0, 0, 0}}, {"math", "value", 1, Vector4{.5, .5, .5, .5}}
	};
	document.Nodes.push_back({"instance", "pc.pixel_math", "", {}, {}});
	document.Nodes.back().InstanceBase = "math";
	document.Outputs = {{"out", "instance", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", FloatImage({.25, .25, .25, .25})}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image first, later;
	REQUIRE(Evaluate(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(Evaluate(restored, plan, "out", request, later, diagnostic) == Status::Ok);
	CHECK(Pixel(first) == SurfacePixel{.25, .25, .25, .25});
	CHECK(Pixel(later) == SurfacePixel{.75, .75, .75, .75});
}
TEST_CASE("Pixel Math selected array rows keep their own operands", "[source_2d][pixel_math]") {
	auto document = Graph();
	ArrayValue values;
	values.ElementType = ValueType::Any;
	values.Items = {
		SourceArrayItem{ElementValue{Vector4{0, 0, 0, 0}}},
		SourceArrayItem{ElementValue{Vector4{.5, .5, .5, .5}}}
	};
	document.Junctions = {{"operands", "", ValueType::Array, values}};
	document.Links.push_back({"operands", "value", "math", "value"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", FloatImage({.25, .25, .25, .25})}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(Pixel(output.Images[0]) == SurfacePixel{.25, .25, .25, .25});
	CHECK(Pixel(output.Images[1]) == SurfacePixel{.75, .75, .75, .75});
}
TEST_CASE("Pixel Math full batch refusal executes no rows", "[source_2d][pixel_math]") {
	const auto source = FloatImage({.25, .25, .25, .25}, 32, 32);
	const auto *entry = FindCatalogueEntry("pc.pixel_math");
	REQUIRE(entry);
	Node node{"math", "pc.pixel_math", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	ArrayValue operands;
	operands.ElementType = ValueType::Any;
	operands.Items.assign(Limits::MaximumArrayElements, SourceArrayItem{ElementValue{Vector4{}}});
	context.Values = {{"operator", EnumValue{0}}, {"value", std::move(operands)}};
	context.InputProvenanceResolved = true;
	size_t observed = 0;
	const auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	const auto execute = detail::FindExecutor("pc.pixel_math");
	REQUIRE(execute);
	CHECK_FALSE(detail::RunProcessorBatch(context, execute, observer, &observed));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete batch") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
}

TEST_CASE(
	"Pixel Math unwraps main SurfaceAtlas while refusing raw operand Atlas", "[source_2d][pixel_math]"
) {
	const auto source = FloatImage({.25, .25, .25, .25});
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = source;
	data.Dimension = {1, 1};
	auto main = RunNode("pc.pixel_math", {}, {{"surface_in", atlas}, {"value", Vector4{.25, .25, .25, .25}}});
	INFO(main.Message);
	REQUIRE(main.Ok);
	CHECK(Pixel(main.Output()) == SurfacePixel{.5, .5, .5, .5});
	auto operand = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}},
		{{"operand_type", EnumValue{1}}, {"operand_surface", atlas}}
	);
	CHECK_FALSE(operand.Ok);
	CHECK(operand.Port == "operand_surface");
}
TEST_CASE("Pixel Math quotes large mask feather work before creating output", "[source_2d][pixel_math]") {
	const auto source = FloatImage({.25, .25, .25, .25});
	const auto mask = FloatImage({1, 1, 1, 1}, 256, 256);
	auto run = RunNode("pc.pixel_math", {{"surface_in", &source}, {"mask", &mask}}, {{"mask_feather", 100.}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::LimitExceeded);
	CHECK(run.Message.find("complete batch") != std::string::npos);
	CHECK(run.Images.empty());
	auto finite = RunNode(
		"pc.pixel_math",
		{{"surface_in", &source}, {"mask", &source}},
		{{"mask_feather", 1.}, {"value", Vector4{.25, .25, .25, .25}}}
	);
	REQUIRE(finite.Ok);
	CHECK(Pixel(finite.Output()) == SurfacePixel{.265625, .265625, .265625, .265625});
}
