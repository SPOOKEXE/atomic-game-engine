#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_displace_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Labels(uint32_t width, uint32_t height = 1) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				image.Pixels[(size_t(y) * width + x) * 4] = uint8_t(10 * (x + 1) + y * 40);
				image.Pixels[(size_t(y) * width + x) * 4 + 3] = 255;
			}
		return image;
	}
	Image White() {
		return {1, 1, {255, 255, 255, 255}, 0};
	}
	uint8_t Red(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		return image.Pixels[(size_t(y) * image.Width + x) * 4];
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"map", "image.captured", "", {}, {{"source_id", std::string("map")}}},
			{"displace",
			 "pc.displace",
			 "",
			 {},
			 {{"position", Vector2{1, 0}},
			  {"position_unit", EnumValue{0}},
			  {"mid_value", 0.},
			  {"interpolate", EnumValue{1}},
			  {"oversample", EnumValue{3}}}}
		};
		document.Links = {
			{"source", "image", "displace", "surface_in"}, {"map", "image", "displace", "displace_map"}
		};
		document.Outputs = {{"out", "displace", "surface_out"}};
		return document;
	}
}
TEST_CASE(
	"Displace reference position uses first source row across differing image dimensions",
	"[source_2d][displace]"
) {
	const auto small = Labels(2), large = Labels(4), map = White();
	const auto *entry = FindCatalogueEntry("pc.displace");
	REQUIRE(entry);
	Node node{"displace", "pc.displace", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ImageArray sources;
	sources.Images = {small, large};
	sources.Items = {{size_t(0)}, {size_t(1)}};
	context.ImageArrays = {{"surface_in", &sources}};
	context.Images = {{"displace_map", &map}};
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	for (auto &[port, value] : context.Values) {
		if (port == "position") value = Vector2{.5, 0};
		if (port == "mid_value") value = 0.;
		if (port == "interpolate") value = EnumValue{1};
		if (port == "oversample") value = EnumValue{3};
	}
	context.InputProvenanceResolved = true;
	REQUIRE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.displace")));
	REQUIRE(context.OutputImageArrays.size() == 1);
	const auto &out = context.OutputImageArrays.front().second;
	REQUIRE(out.Images.size() == 2);
	CHECK(Red(out.Images[0]) == 20);
	CHECK(Red(out.Images[1]) == 20);
	CHECK_FALSE(context.DisplaceReferenceDimension.has_value());
	const auto ownReference = RunNode(
		"pc.displace",
		{{"surface_in", &large}, {"displace_map", &map}},
		{{"position", Vector2{.5, 0}},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	REQUIRE(ownReference.Ok);
	CHECK(Red(ownReference.Output()) == 30);
}
TEST_CASE(
	"Displace whole batch refuses cumulative work and later overflow before observers",
	"[source_2d][displace]"
) {
	Image source{400, 400, std::vector<uint8_t>(400 * 400 * 4, 255), 0};
	const auto map = White();
	const auto *entry = FindCatalogueEntry("pc.displace");
	REQUIRE(entry);
	Node node{"displace", "pc.displace", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}, {"displace_map", &map}};
	context.DisplaceReferenceDimension = Vector2{7, 9};
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	for (auto &[port, value] : context.Values) {
		if (port == "strength") value = ArrayValue{ValueType::Scalar, {0., 1.}};
		if (port == "interpolate") value = EnumValue{1};
	}
	Status expected = Status::LimitExceeded;
	std::string_view port = "surface_out";
	SECTION("cumulative sampling work") {}
	SECTION("later coordinate overflow") {
		source = Labels(2);
		for (auto &[id, value] : context.Values) {
			if (id == "strength") value = ArrayValue{ValueType::Scalar, {0., 4.}};
			if (id == "position") value = Vector2{std::numeric_limits<float>::max(), 0};
			if (id == "position_unit") value = EnumValue{0};
			if (id == "mid_value") value = 0.;
		}
		expected = Status::InvalidValue;
		port = "strength";
	}
	context.InputProvenanceResolved = true;
	size_t observed = 0;
	const auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.displace"), observer, &observed));
	CHECK(context.FailureCode == expected);
	CHECK(context.FailurePort == port);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.DisplaceReferenceDimension == Vector2{7, 9});
}
TEST_CASE(
	"Displace linked pixels, Expand rows and animation survive document round trips", "[source_2d][displace]"
) {
	const auto source = Labels(4), map = White();
	auto document = Graph();
	for (auto &value : document.Nodes.back().Values)
		if (value.Port == "position_unit") value.Data = EnumValue{1};
	document.Junctions = {
		{"position", "", ValueType::Vector2, Vector2{1, 0}},
		{"strength", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 1.}}}
	};
	document.Links.push_back({"position", "value", "displace", "position"});
	document.Links.push_back({"strength", "value", "displace", "strength"});
	document.Nodes.back().Values.push_back({"attribute_array_process", EnumValue{2}});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 2> sources{{{"source", source}, {"map", map}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(restored, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(Red(output.Images[0]) == 10);
	CHECK(Red(output.Images[1]) == 20);
	document = Graph();
	document.Nodes.back().SourceAnimatedInputs = {"strength"};
	document.Keyframes = {{"displace", "strength", 0, 0.}, {"displace", "strength", 1, 1.}};
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image first, later;
	request.Tick = 0;
	REQUIRE(Evaluate(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(Evaluate(restored, plan, "out", request, later, diagnostic) == Status::Ok);
	CHECK(Red(first) == 10);
	CHECK(Red(later) == 20);
}
TEST_CASE("Displace UV alpha and legacy oversample mode remain source-ignored", "[source_2d][displace]") {
	const auto source = Labels(2, 2), map = White();
	Image uv{1, 1, {0, 255, 0, 0}, 0};
	const auto noMap = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"strength", 0.}, {"interpolate", EnumValue{1}}}
	);
	const auto zeroMix = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}, {"uv_map", &uv}},
		{{"strength", 0.}, {"uv_mix", 0.}, {"interpolate", EnumValue{1}}, {"oversample_mode", EnumValue{2}}}
	);
	REQUIRE(noMap.Ok);
	REQUIRE(zeroMix.Ok);
	CHECK(zeroMix.Output() == noMap.Output());
	const auto remapped = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}, {"uv_map", &uv}},
		{{"strength", 0.}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(remapped.Ok);
	for (size_t pixel = 0; pixel < 4; ++pixel) {
		CHECK(remapped.Output().Pixels[pixel * 4] == 10);
		CHECK(remapped.Output().Pixels[pixel * 4 + 3] == 255);
	}
}
TEST_CASE("Displace main Atlas unwraps but raw UV Atlas refuses explicitly", "[source_2d][displace]") {
	const auto source = Labels(2, 2), map = White();
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = source;
	data.Dimension = {1, 1};
	const auto native = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"strength", 0.}, {"interpolate", EnumValue{1}}}
	);
	const auto unwrapped = RunNode(
		"pc.displace",
		{{"displace_map", &map}},
		{{"surface_in", atlas}, {"strength", 0.}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(native.Ok);
	REQUIRE(unwrapped.Ok);
	CHECK(unwrapped.Output() == native.Output());
	const auto refused =
		RunNode("pc.displace", {{"surface_in", &source}, {"displace_map", &map}}, {{"uv_map", atlas}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Port == "uv_map");
	CHECK(refused.Images.empty());
}

TEST_CASE(
	"Displace separated axes, luma blends and empty iteration follow source branches", "[source_2d][displace]"
) {
	const auto source = Labels(4), white = White();
	const Image black{1, 1, {0, 0, 0, 255}, 0};
	const auto vector = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &white}, {"displace_map_2", &black}},
		{{"mode", EnumValue{1}},
		 {"separate_axis", true},
		 {"strength", .25},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	const auto angle = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &black}, {"displace_map_2", &white}},
		{{"mode", EnumValue{2}},
		 {"separate_axis", true},
		 {"strength", .25},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	REQUIRE(vector.Ok);
	REQUIRE(angle.Ok);
	CHECK(Red(vector.Output()) == 20);
	CHECK(Red(angle.Output()) == 20);
	const auto minimum = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &white}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"blend_mode", EnumValue{1}},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	const auto maximum = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &white}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"blend_mode", EnumValue{2}},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	REQUIRE(minimum.Ok);
	REQUIRE(maximum.Ok);
	CHECK(Red(minimum.Output()) == 10);
	CHECK(Red(maximum.Output()) == 20);
	for (int64_t steps : {int64_t(0), int64_t(-1)}) {
		const auto emptyLoop = RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &white}},
			{{"iterate", true}, {"iteration", steps}, {"interpolate", EnumValue{1}}}
		);
		REQUIRE(emptyLoop.Ok);
		CHECK(emptyLoop.Output().Pixels == source.Pixels);
	}
	const auto separator =
		RunNode("pc.displace", {{"surface_in", &source}, {"displace_map", &white}}, {{"mode", EnumValue{4}}});
	CHECK_FALSE(separator.Ok);
	CHECK(separator.Code == Status::UnsupportedExecution);
	CHECK(separator.Port == "mode");
	const auto huge = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &white}},
		{{"repeat", std::numeric_limits<int64_t>::max()}}
	);
	CHECK_FALSE(huge.Ok);
	CHECK(huge.Code == Status::LimitExceeded);
	CHECK(huge.Images.empty());
}

TEST_CASE(
	"Displace scratch conversion precedes typed processor finish and requested depth", "[source_2d][displace]"
) {
	Image source{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {2, -.25, .25, 1}));
	const auto map = White();
	for (int64_t depth : {int64_t(3), int64_t(4), int64_t(5)}) {
		const auto run = RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &map}},
			{{"strength", 0.}, {"attribute_color_depth", EnumValue{depth}}, {"interpolate", EnumValue{1}}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *SourceSurfaceFormat(depth));
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
		CHECK(pixel[0] == 1.);
		CHECK(pixel[1] == 0.);
	}
	const auto restored = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"strength", 0.}, {"mix", 0.}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(restored.Ok);
	CHECK(restored.Output().Pixels == source.Pixels);
	const auto scalarMapped = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}, {"strength_map", &map}},
		{{"strength", 0.}, {"strength_mapped", true}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(scalarMapped.Ok);
	SurfacePixel pixel{};
	REQUIRE(LoadSurfacePixel(scalarMapped.Output(), 0, 0, pixel));
	CHECK(pixel[0] == 1.);
}
