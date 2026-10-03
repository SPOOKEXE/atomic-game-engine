#include "nodes/SourceErodeNodes.hpp"

#include "ErodeReference.hpp"
#include "NodeHarness.hpp"
#include "ProcessorBatch.hpp"
#include "nodes/Processor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_erode")
using namespace engine::imagegraph;
namespace {
	Image Source() {
		return {5, 5, {erode_reference::INPUT.begin(), erode_reference::INPUT.end()}};
	}
	Document Graph(std::vector<AuthoredValue> values = {}) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"capture", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
			{"erode", "pc.erode", "", {}, std::move(values)}
		};
		d.Links = {{"capture", "image", "erode", "surface_in"}};
		d.Outputs = {{"output", "erode", "surface_out"}};
		return d;
	}
	Image Eval(Document &d, const EvaluationRequest &request) {
		Plan p;
		Diagnostic diag;
		const auto compile = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(compile == Status::Ok);
		Image out;
		const auto status = Evaluate(d, p, "output", request, out, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return out;
	}
}
TEST_CASE("Source erosion and signed expansion preserve four shader scan orders", "[source_erode]") {
	RequestImageSource source{"input", Source()};
	EvaluationRequest request;
	request.ImageSources = {&source, 1};
	for (int mode = 0; mode < 4; ++mode)
		for (int sign : {1, -1}) {
			CAPTURE(mode, sign);
			auto d =
				Graph({{"width", int64_t(sign)}, {"pattern", EnumValue{mode}}, {"oversample", EnumValue{1}}});
			const auto out = Eval(d, request);
			const size_t index = size_t(mode) * 2 + (sign < 0);
			CHECK(
				out.Pixels == std::vector<uint8_t>(
								  erode_reference::GOLDEN[index].begin(), erode_reference::GOLDEN[index].end()
							  )
			);
			Document restored;
			Diagnostic diag;
			REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
			CHECK(Eval(restored, request).Pixels == out.Pixels);
			d.Nodes[1].Values.push_back({"preserve_border", true});
			d.Nodes[1].Values.push_back({"use_alpha", false});
			CHECK(Eval(d, request).Pixels == out.Pixels);
		}
}
TEST_CASE(
	"Erode source cross takes the first strictly greater alpha and ignores synthetic interpolation",
	"[source_erode]"
) {
	Image source{3, 3, std::vector<uint8_t>(36)};
	detail::WritePixel(source, 2, 1, {0, 1, 0, 1});
	detail::WritePixel(source, 0, 1, {1, 0, 0, 1});
	const auto result = imagegraph_test::RunNode(
		"pc.erode",
		{{"surface_in", &source}},
		{{"width", int64_t{-1}},
		 {"pattern", EnumValue{3}},
		 {"oversample", EnumValue{1}},
		 {"interpolate", EnumValue{4}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(detail::ReadPixel(result.Output(), 1, 1) == detail::Rgba{0, 1, 0, 1});
}
TEST_CASE("Mapped source integer ranges retain nested fractional loop origins", "[source_erode]") {
	Image source = Source(), map{5, 5, std::vector<uint8_t>(100, 128)};
	for (int mode = 1; mode <= 3; ++mode) {
		const auto run = imagegraph_test::RunNode(
			"pc.erode",
			{{"surface_in", &source}, {"width_map", &map}},
			{{"width_mapped", true},
			 {"width_map_range", Vector2{1.5, -2.5}},
			 {"pattern", EnumValue{mode}},
			 {"oversample", EnumValue{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(
			run.Output().Pixels ==
			std::vector<uint8_t>(
				erode_reference::GOLDEN[7 + mode].begin(), erode_reference::GOLDEN[7 + mode].end()
			)
		);
	}
}
TEST_CASE("Erode batch width work and output bytes refuse before publishing any row", "[source_erode]") {
	const auto *entry = FindCatalogueEntry("pc.erode");
	REQUIRE(entry);
	Node node{"node", "pc.erode", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	Image source = Source();
	c.Images = {{"surface_in", &source}};
	for (const auto &i : entry->Inputs)
		if (auto d = CatalogueDefault(i)) c.Values.emplace_back(i.Id, *d);
	for (auto &[id, value] : c.Values)
		if (id == "width") value = ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{100000}}};
	CHECK_FALSE(detail::RunProcessorBatch(c, detail::SourceErode));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputImages.empty());
	CHECK(c.OutputImageArrays.empty());
	detail::NodeContext tight(node, *entry, request);
	tight.ByteBudget = 32;
	tight.Images = {{"surface_in", &source}};
	CHECK_FALSE(detail::SourceErode(tight));
	CHECK(tight.FailureCode == Status::LimitExceeded);
	CHECK(tight.OutputImages.empty());
}
TEST_CASE(
	"Erode mask channel and inactive source retain postprocessor and refusal contracts", "[source_erode]"
) {
	Image source = Source(), mask{1, 1, {0, 0, 0, 255}};
	const auto masked = imagegraph_test::RunNode(
		"pc.erode",
		{{"surface_in", &source}, {"mask", &mask}},
		{{"pattern", EnumValue{1}}, {"oversample", EnumValue{1}}}
	);
	INFO(masked.Message);
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == source.Pixels);
	const auto channel = imagegraph_test::RunNode(
		"pc.erode", {{"surface_in", &source}}, {{"channel", int64_t{7}}, {"oversample", EnumValue{1}}}
	);
	REQUIRE(channel.Ok);
	CHECK(channel.Output().Pixels == source.Pixels);
	const auto inactive = imagegraph_test::RunNode(
		"pc.erode", {{"surface_in", &source}}, {{"active", false}, {"width", int64_t{999999999}}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output() == source);
	RequestImageSource capture{"input", source};
	EvaluationRequest request;
	request.ImageSources = {&capture, 1};
	request.RequireSourceGpuRasterCoverage = true;
	auto d = Graph();
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image sentinel{1, 1, {7, 11, 13, 17}}, out = sentinel;
	CHECK(Evaluate(d, p, "output", request, out, diag) == Status::UnsupportedExecution);
	CHECK(out == sentinel);
}
TEST_CASE(
	"Compiled Erode arrays and animated widths retain exact getter and save semantics", "[source_erode]"
) {
	RequestImageSource source{"input", Source()};
	EvaluationRequest request;
	request.ImageSources = {&source, 1};
	auto d = Graph(
		{{"width", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{-1}}}},
		 {"pattern", EnumValue{1}},
		 {"oversample", EnumValue{1}}}
	);
	Plan p;
	Diagnostic diag;
	const auto compile = Compile(d, p, diag);
	INFO(diag.Message);
	REQUIRE(compile == Status::Ok);
	ImageArray array;
	const auto status = EvaluateArray(d, p, "output", request, array, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(array.Images.size() == 2);
	CHECK(
		array.Images[0].Pixels ==
		std::vector<uint8_t>(erode_reference::GOLDEN[2].begin(), erode_reference::GOLDEN[2].end())
	);
	CHECK(
		array.Images[1].Pixels ==
		std::vector<uint8_t>(erode_reference::GOLDEN[3].begin(), erode_reference::GOLDEN[3].end())
	);
	d.Nodes[1].Values[0].Data = int64_t{1};
	d.Keyframes = {{"erode", "width", 0, int64_t{1}, "linear"}, {"erode", "width", 2, int64_t{-1}, "step"}};
	request.Tick = 0;
	const auto eroded = Eval(d, request);
	request.Tick = 2;
	const auto expanded = Eval(d, request);
	CHECK(eroded.Pixels == array.Images[0].Pixels);
	CHECK(expanded.Pixels == array.Images[1].Pixels);
	Document restored;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Eval(restored, request).Pixels == expanded.Pixels);
}
TEST_CASE("Compiled mapped Erode rounds flat endpoints before retaining a single range", "[source_erode]") {
	RequestImageSource sources[2]{{"input", Source()}, {"map", Image{5, 5, std::vector<uint8_t>(100, 128)}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	auto d = Graph(
		{{"width_mapped", true},
		 {"width_map_range", Vector2{1.5, -2.5}},
		 {"pattern", EnumValue{1}},
		 {"oversample", EnumValue{1}}}
	);
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}});
	d.Links.push_back({"map", "image", "erode", "width_map"});
	const auto rounded = Eval(d, request);
	d.Nodes[1].Values[1].Data = Vector2{2, -2};
	CHECK(Eval(d, request).Pixels == rounded.Pixels);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Eval(restored, request).Pixels == rounded.Pixels);
}
TEST_CASE(
	"Compiled nested mapped Erode keeps fractional endpoints through source row selection", "[source_erode]"
) {
	RequestImageSource sources[2]{{"input", Source()}, {"map", Image{5, 5, std::vector<uint8_t>(100, 128)}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	auto d = Graph(
		{{"width_mapped", true},
		 {"width_map_range", ArrayValue{ValueType::Vector2, {Vector2{1.5, -2.5}}}},
		 {"pattern", EnumValue{1}},
		 {"oversample", EnumValue{1}}}
	);
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}});
	d.Links.push_back({"map", "image", "erode", "width_map"});
	const auto out = Eval(d, request);
	CHECK(
		out.Pixels ==
		std::vector<uint8_t>(erode_reference::GOLDEN[8].begin(), erode_reference::GOLDEN[8].end())
	);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Eval(restored, request).Pixels == out.Pixels);
}
TEST_CASE("Erode sampler_simple preserves every documented oversampling branch", "[source_erode]") {
	const Image source{1, 1, {7, 11, 13, 255}};
	for (int mode : {1, 2, 3, 4, 6, 7, 8, 10, 11, 12}) {
		CAPTURE(mode);
		const auto run = imagegraph_test::RunNode(
			"pc.erode",
			{{"surface_in", &source}},
			{{"pattern", EnumValue{3}}, {"oversample", EnumValue{mode}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(
			run.Output().Pixels ==
			std::vector<uint8_t>{7, 11, 13, uint8_t(mode == 1 || mode == 6 || mode == 10 ? 0 : 255)}
		);
	}
}
TEST_CASE("Source Erode retains signed floating RGB and sampled negative alpha", "[source_erode]") {
	Image source{3, 1, std::vector<uint8_t>(48), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {0, 0, 0, -.5}));
	REQUIRE(StoreSurfacePixel(source, 1, 0, {-2, 4, .25, 1}));
	REQUIRE(StoreSurfacePixel(source, 2, 0, {1, 2, 3, 1}));
	const auto run = imagegraph_test::RunNode(
		"pc.erode",
		{{"surface_in", &source}},
		{{"pattern", EnumValue{3}}, {"oversample", EnumValue{3}}, {"use_alpha", false}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(detail::ReadPixel(run.Output(), 1, 0) == detail::Rgba{-2, 4, .25, -.5});
}

TEST_CASE(
	"Erode aggregate admission counts base map and write work with inert Lanczos metadata", "[source_erode]"
) {
	RequestImageSource sources[2]{
		{"input", Image{1, 1, {7, 11, 13, 255}}}, {"map", Image{1, 1, {128, 128, 128, 255}}}
	};
	EvaluationRequest request;
	request.ImageSources = sources;
	// Force an early named source-GPU refusal if the aggregate admission mistakenly succeeds.
	request.RequireSourceGpuRasterCoverage = true;
	auto d = Graph(
		{{"width_mapped", true},
		 {"width_map_range",
		  ArrayValue{ValueType::Vector2, {Vector2{5333333, 5333333}, Vector2{1, 1}, Vector2{1, 1}}}},
		 {"pattern", EnumValue{3}},
		 {"oversample", EnumValue{4}},
		 {"interpolate", EnumValue{4}}}
	);
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}});
	d.Links.push_back({"map", "image", "erode", "width_map"});
	Plan p;
	Diagnostic diag;
	const auto compiled = Compile(d, p, diag);
	INFO(diag.Message);
	REQUIRE(compiled == Status::Ok);
	// Offset+base alone is63,999,999; three mapped reads and three writes exceed64,000,000.
	ImageArray sentinel;
	sentinel.Images.push_back({1, 1, {17, 19, 23, 29}});
	sentinel.Items.push_back({size_t{0}});
	const auto before = sentinel;
	const auto status = EvaluateArray(d, p, "output", request, sentinel, diag);
	INFO(diag.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(diag.NodeId == "erode");
	CHECK(diag.Port == "width");
	CHECK(sentinel.Images == before.Images);
	REQUIRE(sentinel.Items.size() == before.Items.size());
	CHECK(std::get<size_t>(sentinel.Items[0].Data) == std::get<size_t>(before.Items[0].Data));
	d.Nodes[1].Values[1].Data =
		ArrayValue{ValueType::Vector2, {Vector2{5333332, 5333332}, Vector2{1, 1}, Vector2{1, 1}}};
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	CHECK(EvaluateArray(d, p, "output", request, sentinel, diag) == Status::UnsupportedExecution);
	CHECK(sentinel.Images == before.Images);
	REQUIRE(sentinel.Items.size() == before.Items.size());
	CHECK(std::get<size_t>(sentinel.Items[0].Data) == std::get<size_t>(before.Items[0].Data));
}

TEST_CASE(
	"Erode batch admission includes a later floating source before publishing the first byte row",
	"[source_erode]"
) {
	RequestImageSource sources[2]{
		{"byte", Image{16, 16, std::vector<uint8_t>(16 * 16 * 4, 255)}},
		{"float", Image{16, 16, std::vector<uint8_t>(16 * 16 * 16), 0, SurfaceFormat::RGBA32Float}}
	};
	Node collect{"collect", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	collect.DynamicInputs = {
		{"input_0", ValueType::Image, std::nullopt}, {"input_1", ValueType::Image, std::nullopt}
	};
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"byte", "image.captured", "", {}, {{"source_id", std::string{"byte"}}}},
		{"float", "image.captured", "", {}, {{"source_id", std::string{"float"}}}},
		std::move(collect),
		{"erode", "pc.erode", "", {}, {{"width", int64_t{0}}, {"attribute_color_depth", EnumValue{0}}}}
	};
	document.Links = {
		{"byte", "image", "collect", "input_0"},
		{"float", "image", "collect", "input_1"},
		{"collect", "array", "erode", "surface_in"}
	};
	document.Outputs = {{"sources", "collect", "array"}, {"output", "erode", "surface_out"}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray sourceArray, output;
	REQUIRE(EvaluateArray(document, plan, "sources", request, sourceArray, diagnostic) == Status::Ok);
	REQUIRE(EvaluateArray(document, plan, "output", request, output, diagnostic) == Status::Ok);
	REQUIRE(sourceArray.Images.size() == 2);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Format == SurfaceFormat::RGBA8Unorm);
	CHECK(output.Images[1].Format == SurfaceFormat::RGBA32Float);
	CHECK(output.Images[0].Pixels.size() == 1024);
	CHECK(output.Images[1].Pixels.size() == 4096);
	for (size_t row = 0; row < sourceArray.Images.size(); ++row) {
		Image captured = sources[row].Data;
		captured.Hash = SurfaceHash(captured);
		REQUIRE(sourceArray.Images[row] == captured);
	}
	const auto sourceBefore = sourceArray.Images;

	const auto *entry = FindCatalogueEntry("pc.erode");
	REQUIRE(entry);
	// Input depth preserves each source format. One byte row fits, but admission
	// must include the later floating row even when every row is disabled.
	for (bool active : {true, false}) {
		detail::NodeContext context(document.Nodes.back(), *entry, request);
		context.ByteBudget = 8191;
		context.ProcessorCount = 2;
		context.Images = {{"surface_in", &sourceArray.Images[0]}};
		context.ImageArrays = {{"surface_in", &sourceArray}};
		context.Values = {{"width", int64_t{0}}, {"attribute_color_depth", EnumValue{0}}, {"active", active}};
		CHECK_FALSE(detail::SourceErode(context));
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.FailurePort == "surface_out");
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(sourceArray.Images[0] == sourceBefore[0]);
		CHECK(sourceArray.Images[1] == sourceBefore[1]);
	}
}
