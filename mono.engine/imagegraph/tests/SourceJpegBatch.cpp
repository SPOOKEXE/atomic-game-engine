#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_jpeg_batch")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Rgba(
		uint32_t width, uint32_t height, uint8_t red, uint8_t green = 0, uint8_t blue = 0, uint8_t alpha = 255
	) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4) {
			image.Pixels[offset] = red;
			image.Pixels[offset + 1] = green;
			image.Pixels[offset + 2] = blue;
			image.Pixels[offset + 3] = alpha;
		}
		return image;
	}
	uint8_t Red(const Image &image, size_t pixel = 0) {
		return image.Pixels[pixel * 4];
	}
	uint8_t Alpha(const Image &image, size_t pixel = 0) {
		return image.Pixels[pixel * 4 + 3];
	}
	const CatalogueEntry *JpegEntry() {
		return FindCatalogueEntry("pc.jpeg");
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
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"jpeg", "pc.jpeg", "", {}, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}}}
		};
		document.Links = {{"source", "image", "jpeg", "surface_in"}};
		document.Outputs = {{"out", "jpeg", "surface_out"}};
		return document;
	}
}

TEST_CASE("JPEG batch admits selected 100x100 rows and preflights two rows", "[source_2d][jpeg]") {
	const auto source = Rgba(100, 100, 32);
	auto setUp = [&](detail::NodeContext &context, ImageArray &values) {
		REQUIRE(JpegEntry());
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		values.Images = {source};
		values.Items = {{size_t(0)}};
		context.ImageArrays = {{"surface_in", &values}};
		SetDefaults(context);
		context.InputProvenanceResolved = true;
		for (auto &[port, value] : context.Values) {
			if (port == "patch_size") value = int64_t{8};
			if (port == "reconstruction") value = int64_t{8};
		}
		SetValue(context, "attribute_array_process", EnumValue{2});
	};
	SECTION("one selected compression row") {
		const auto *entry = JpegEntry();
		REQUIRE(entry);
		ImageArray values;
		Node node{"jpeg", "pc.jpeg", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		setUp(context, values);
		SetValue(context, "compression", ArrayValue{ValueType::Scalar, {0.}});
		size_t observed = 0;
		REQUIRE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.jpeg"), Observe, &observed));
		CHECK(observed == 1);
		REQUIRE(context.OutputImages.size() == 1);
		CHECK(context.OutputImages.front().second.Width == 100);
		CHECK(context.OutputImages.front().second.Height == 100);
	}
	SECTION("two selected reconstruction rows exceed complete work limit before observer") {
		const auto *entry = JpegEntry();
		REQUIRE(entry);
		ImageArray values;
		Node node{"jpeg", "pc.jpeg", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		setUp(context, values);
		SetValue(context, "reconstruction", ArrayValue{ValueType::Scalar, {8., 8.}});
		size_t observed = 0;
		CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.jpeg"), Observe, &observed));
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.FailurePort == "surface_out");
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
}

TEST_CASE(
	"JPEG batch rejects later shader overflow, signed counters and nonfinite phase before observer",
	"[source_2d][jpeg]"
) {
	const auto source = Rgba(1, 1, 255);
	auto runRefusal = [&](std::string_view port, Status expected, auto configure) {
		const auto *entry = JpegEntry();
		REQUIRE(entry);
		Node node{"jpeg", "pc.jpeg", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"surface_in", &source}};
		SetDefaults(context);
		SetValue(context, "attribute_process", true);
		SetValue(context, "attribute_array_process", EnumValue{2});
		for (auto &[id, value] : context.Values)
			configure(id, value);
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.jpeg"), Observe, &observed));
		CHECK(context.FailureCode == expected);
		CHECK(context.FailurePort == port);
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
	};
	SECTION("later compression conversion overflow") {
		runRefusal("compression", Status::InvalidValue, [](std::string_view id, Value &value) {
			if (id == "compression")
				value = ArrayValue{ValueType::Scalar, {0., std::numeric_limits<double>::max()}};
		});
	}
	SECTION("int32 shader counter overflow") {
		runRefusal("patch_size", Status::LimitExceeded, [](std::string_view id, Value &value) {
			if (id == "patch_size") value = std::numeric_limits<int64_t>::max();
		});
	}
	SECTION("nonfinite phase") {
		runRefusal("phase", Status::InvalidValue, [](std::string_view id, Value &value) {
			if (id == "phase") value = std::numeric_limits<double>::infinity();
		});
	}
}

TEST_CASE(
	"JPEG linked Expand arrays and animation phase survive native document evaluation", "[source_2d][jpeg]"
) {
	const auto source = Rgba(1, 1, 64, 31, 18, 73);
	auto document = Graph();
	document.Junctions = {{"compression", "", ValueType::Array, ArrayValue{ValueType::Scalar, {0., 1.}}}};
	document.Links.push_back({"compression", "value", "jpeg", "compression"});
	document.Nodes.back().Values.push_back({"attribute_array_process", EnumValue{2}});
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
	INFO(diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(Red(output.Images[0]) == 64);
	CHECK(Red(output.Images[1]) == 0);
	CHECK(Alpha(output.Images[0]) == 255);
	CHECK(Alpha(output.Images[1]) == 255);

	document = Graph();
	document.Nodes.back().SourceAnimatedInputs = {"phase"};
	document.Keyframes = {{"jpeg", "phase", 0, 0.}, {"jpeg", "phase", 1, 90.}};
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image zeroPhase, quarterTurn;
	request.Tick = 0;
	REQUIRE(Evaluate(restored, plan, "out", request, zeroPhase, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(Evaluate(restored, plan, "out", request, quarterTurn, diagnostic) == Status::Ok);
	CHECK(Red(zeroPhase) == 64);
	CHECK(Red(quarterTurn) == 0);
}

TEST_CASE("JPEG unwraps its main SurfaceAtlas and refuses an Atlas raw mask", "[source_2d][jpeg]") {
	const auto source = Rgba(1, 1, 64, 32, 16, 80);
	AtlasValue atlas;
	auto &data = atlas.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = source;
	data.Dimension = {1, 1};
	const auto native = RunNode(
		"pc.jpeg", {{"surface_in", &source}}, {{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}}
	);
	const auto unwrapped = RunNode(
		"pc.jpeg", {}, {{"surface_in", atlas}, {"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}}
	);
	INFO(native.Message);
	INFO(unwrapped.Message);
	REQUIRE(native.Ok);
	REQUIRE(unwrapped.Ok);
	CHECK(unwrapped.Output() == native.Output());
	const auto refused = RunNode("pc.jpeg", {{"surface_in", &source}}, {{"mask", atlas}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Port == "mask");
	CHECK(refused.Images.empty());
}

TEST_CASE(
	"JPEG finish applies mask mix and channel selection while preserving typed output", "[source_2d][jpeg]"
) {
	const auto source = Rgba(1, 1, 64, 32, 16, 80);
	const auto black = Rgba(1, 1, 0);
	const auto redOnly = RunNode(
		"pc.jpeg",
		{{"surface_in", &source}},
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{0}},
		 {"deconstruct_only", true},
		 {"channel", int64_t{1}}}
	);
	const auto mixed = RunNode(
		"pc.jpeg",
		{{"surface_in", &source}},
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{0}},
		 {"deconstruct_only", true},
		 {"mix", 0.},
		 {"channel", int64_t{15}}}
	);
	const auto masked = RunNode(
		"pc.jpeg",
		{{"surface_in", &source}, {"mask", &black}},
		{{"patch_size", int64_t{1}}, {"reconstruction", int64_t{0}}, {"deconstruct_only", true}}
	);
	const auto invertedMask = RunNode(
		"pc.jpeg",
		{{"surface_in", &source}, {"mask", &black}},
		{{"patch_size", int64_t{1}},
		 {"reconstruction", int64_t{0}},
		 {"deconstruct_only", true},
		 {"invert_mask", true}}
	);
	INFO(redOnly.Message);
	INFO(mixed.Message);
	INFO(masked.Message);
	INFO(invertedMask.Message);
	REQUIRE(redOnly.Ok);
	REQUIRE(mixed.Ok);
	REQUIRE(masked.Ok);
	REQUIRE(invertedMask.Ok);
	CHECK(redOnly.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(Red(redOnly.Output()) == 0);
	CHECK(redOnly.Output().Pixels[1] == 32);
	CHECK(redOnly.Output().Pixels[2] == 16);
	CHECK(Alpha(redOnly.Output()) == 80);
	CHECK(mixed.Output().Pixels == source.Pixels);
	CHECK(masked.Output().Pixels == source.Pixels);
	CHECK(invertedMask.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	for (int64_t depth : {int64_t{3}, int64_t{4}, int64_t{5}}) {
		const auto typed = RunNode(
			"pc.jpeg",
			{{"surface_in", &source}},
			{{"patch_size", int64_t{1}},
			 {"reconstruction", int64_t{0}},
			 {"deconstruct_only", true},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(typed.Message);
		REQUIRE(typed.Ok);
		CHECK(typed.Output().Format == *SourceSurfaceFormat(depth));
	}
}

TEST_CASE("JPEG reserves typed scratch before publishing output", "[source_2d][jpeg]") {
	const auto source = Rgba(1, 1, 64);
	const auto *entry = JpegEntry();
	REQUIRE(entry);
	Node node{"jpeg", "pc.jpeg", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.Images = {{"surface_in", &source}};
	SetDefaults(context);
	for (auto &[port, value] : context.Values) {
		if (port == "patch_size") value = int64_t{1};
		if (port == "reconstruction") value = int64_t{1};
	}
	context.InputProvenanceResolved = true;
	const auto executor = detail::FindExecutor("pc.jpeg");
	REQUIRE(executor);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.OutputImages.empty());
}

TEST_CASE("JPEG half-float scratch quantizes HDR before Float32 publication", "[source_2d][jpeg]") {
	Image source{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {2.0019, .5, .25, .3}));
	const auto active = RunNode(
		"pc.jpeg",
		{{"surface_in", &source}},
		{{"patch_size", int64_t{1}}, {"reconstruction", int64_t{1}}, {"compression", 0.}}
	);
	INFO(active.Message);
	REQUIRE(active.Ok);
	CHECK(active.Output().Format == SurfaceFormat::RGBA32Float);
	SurfacePixel pixel{};
	REQUIRE(LoadSurfacePixel(active.Output(), 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(2.001953125).margin(.00001));
	CHECK(pixel[0] != Catch::Approx(2.0019).margin(.00001));
}
