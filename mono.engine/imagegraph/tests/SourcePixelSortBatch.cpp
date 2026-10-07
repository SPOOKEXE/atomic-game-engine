#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_pixel_sort_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Pixels(uint32_t width, uint32_t height, std::vector<uint8_t> pixels) {
		return Image{width, height, std::move(pixels), 0};
	}
	const CatalogueEntry *SortEntry() {
		return FindCatalogueEntry("pc.pixel_sort");
	}
	void SetDefaults(detail::NodeContext &context) {
		for (const auto &input : context.Entry.Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	}
	void SetValue(detail::NodeContext &context, std::string_view port, Value value) {
		for (auto &[id, current] : context.Values)
			if (id == port) {
				current = std::move(value);
				return;
			}
		context.Values.emplace_back(std::string(port), std::move(value));
	}
	bool Observe(detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	}
	Document SortGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"sort", "pc.pixel_sort", "", {}, {}}
		};
		document.Links = {{"source", "image", "sort", "surface_in"}};
		document.Outputs = {{"out", "sort", "surface_out"}};
		return document;
	}
}

TEST_CASE(
	"Pixel Sort retains borrowed surfaces through mask feather and channel mix", "[source_2d][pixel_sort]"
) {
	const auto source = Pixels(4, 1, {240, 240, 240, 255, 160, 160, 160, 255, 80, 80, 80, 255, 0, 0, 0, 255});
	const auto mask =
		Pixels(4, 1, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255});
	const auto feathered = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &source}, {"mask", &mask}},
		{{"iteration", int64_t{3}},
		 {"threshold", 0.0},
		 {"direction", int64_t{0}},
		 {"mask_feather", 1.0},
		 {"mix", 0.5},
		 {"channel", int64_t{5}}}
	);
	INFO(feathered.Message);
	REQUIRE(feathered.Ok);
	REQUIRE(feathered.Images.size() == 1);
	CHECK(feathered.Output().Width == source.Width);
	CHECK(feathered.Output().Height == source.Height);
	CHECK(feathered.Output().Pixels.size() == source.Pixels.size());
	CHECK(
		feathered.Output().Pixels ==
		std::vector<uint8_t>{160, 240, 160, 255, 160, 160, 160, 255, 160, 80, 160, 255, 0, 0, 0, 255}
	);
}

TEST_CASE("Pixel Sort linked Expand rows and animated controls survive Format9", "[source_2d][pixel_sort]") {
	const auto source = Pixels(3, 1, {240, 240, 240, 255, 96, 96, 96, 255, 8, 8, 8, 255});
	auto document = SortGraph();
	document.Junctions = {{"iteration", "", ValueType::Array, ArrayValue{ValueType::Integer, {1, 2}}}};
	document.Links.push_back({"iteration", "value", "sort", "iteration"});
	document.Nodes.back().Values.push_back({"attribute_array_process", EnumValue{2}});
	document.Nodes.back().Values.push_back({"threshold", 0.0});
	document.Nodes.back().SourceAnimatedInputs = {"threshold"};
	document.Keyframes = {{"sort", "threshold", 0, 0.0}, {"sort", "threshold", 1, 0.25}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(restored, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Pixels == std::vector<uint8_t>{96, 96, 96, 255, 240, 240, 240, 255, 8, 8, 8, 255});
	CHECK(output.Images[1].Pixels == std::vector<uint8_t>{96, 96, 96, 255, 8, 8, 8, 255, 240, 240, 240, 255});
	request.Tick = 1;
	ImageArray animated;
	REQUIRE(EvaluateArray(restored, plan, "out", request, animated, diagnostic) == Status::Ok);
	REQUIRE(animated.Images.size() == 2);
	CHECK(animated.Images[0].Pixels == output.Images[0].Pixels);
	CHECK(animated.Images[1].Pixels == output.Images[0].Pixels);
}

TEST_CASE("Pixel Sort preflights complete batch work and reports named limits", "[source_2d][pixel_sort]") {
	const auto source = Pixels(1000, 200, std::vector<uint8_t>(800000, 96));
	auto configure = [&](detail::NodeContext &context, ImageArray &images) {
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		images.Images = {source};
		images.Items = {{size_t(0)}, {size_t(0)}};
		context.ImageArrays = {{"surface_in", &images}};
		SetDefaults(context);
		context.InputProvenanceResolved = true;
		SetValue(context, "attribute_array_process", EnumValue{2});
		SetValue(context, "iteration", ArrayValue{ValueType::Integer, {2, 2}});
	};
	const auto *entry = SortEntry();
	REQUIRE(entry);
	Node node{"sort", "pc.pixel_sort", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	ImageArray images;
	configure(context, images);
	size_t observed = 0;
	CHECK_FALSE(
		detail::RunProcessorBatch(context, detail::FindExecutor("pc.pixel_sort"), Observe, &observed)
	);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.FailureMessage == "Pixel Sort complete batch exceeds work limit");
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());

	const auto invalid = RunNode("pc.pixel_sort", {}, {{"threshold", 0.0}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Port == "surface_in");
	const auto mask = Pixels(1, 1, {255, 255, 255, 255});
	const auto excessiveFeather = RunNode(
		"pc.pixel_sort",
		{{"surface_in", &mask}, {"mask", &mask}},
		{{"mask_feather", std::numeric_limits<double>::max()}}
	);
	CHECK_FALSE(excessiveFeather.Ok);
	CHECK(excessiveFeather.Code == Status::LimitExceeded);
	CHECK(excessiveFeather.Port == "mask_feather");
	CHECK(excessiveFeather.Message == "Pixel Sort mask feather exceeds supported radius");

	const auto infiniteThreshold = RunNode(
		"pc.pixel_sort", {{"surface_in", &mask}}, {{"threshold", std::numeric_limits<double>::infinity()}}
	);
	CHECK_FALSE(infiniteThreshold.Ok);
	CHECK(infiniteThreshold.Code == Status::InvalidValue);
	CHECK(infiniteThreshold.Port == "threshold");

	const auto infiniteDirection = RunNode(
		"pc.pixel_sort", {{"surface_in", &mask}}, {{"direction", std::numeric_limits<double>::infinity()}}
	);
	CHECK_FALSE(infiniteDirection.Ok);
	CHECK(infiniteDirection.Code == Status::InvalidValue);
	CHECK(infiniteDirection.Port == "direction");

	const auto infiniteIteration = RunNode(
		"pc.pixel_sort", {{"surface_in", &mask}}, {{"iteration", std::numeric_limits<double>::infinity()}}
	);
	CHECK_FALSE(infiniteIteration.Ok);
	CHECK(infiniteIteration.Code == Status::InvalidValue);
	CHECK(infiniteIteration.Port == "iteration");
}

TEST_CASE("Pixel Sort refuses scratch storage before publishing an output", "[source_2d][pixel_sort]") {
	const auto source = Pixels(1, 1, {96, 96, 96, 255});
	const auto *entry = SortEntry();
	REQUIRE(entry);
	Node node{"sort", "pc.pixel_sort", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.Images = {{"surface_in", &source}};
	SetDefaults(context);
	context.InputProvenanceResolved = true;
	const auto executor = detail::FindExecutor("pc.pixel_sort");
	REQUIRE(executor);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.OutputImages.empty());
}

TEST_CASE(
	"Pixel Sort unwraps a SurfaceAtlas source but rejects an Atlas raw mask", "[source_2d][pixel_sort]"
) {
	const auto source = Pixels(2, 1, {16, 32, 48, 255, 240, 224, 208, 255});
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = source;
	data.Dimension = {2, 1};
	const auto native = RunNode("pc.pixel_sort", {{"surface_in", &source}}, {{"iteration", int64_t{1}}});
	const auto unwrapped = RunNode("pc.pixel_sort", {}, {{"surface_in", atlas}, {"iteration", int64_t{1}}});
	INFO(native.Message);
	INFO(unwrapped.Message);
	REQUIRE(native.Ok);
	REQUIRE(unwrapped.Ok);
	CHECK(unwrapped.Output() == native.Output());
	const auto refused =
		RunNode("pc.pixel_sort", {{"surface_in", &source}}, {{"mask", atlas}, {"iteration", int64_t{1}}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Port == "mask");
	CHECK(refused.Images.empty());
}
