#include "NodeHarness.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_animation")
using namespace engine::imagegraph;
namespace {
	double Scalar(const imagegraph_test::NodeRun &run, std::string_view port = "output") {
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto *value = run.OutputValue(port);
		REQUIRE(value);
		return std::get<double>(*value);
	}
	Image Solid(uint8_t red) {
		Image image{1, 1, {red, 0, 0, 255}};
		image.Hash = SurfaceHash(image);
		return image;
	}
	Document SurfaceGraph(std::string type, std::vector<AuthoredValue> values = {}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
		const bool reverse = type == "pc.revert";
		doc.Nodes = {
			{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
			{"temporal", std::move(type), "", {}, std::move(values)}
		};
		doc.Links = {{"input", "image", "temporal", reverse ? "surface_in" : "surface"}};
		doc.Outputs = {{"out", "temporal", reverse ? "output" : "surface"}};
		return doc;
	}
}
TEST_CASE(
	"source function nodes preserve timeline divisions easing and waveform selection",
	"[imagegraph][source_animation]"
) {
	const TimelineSettings timeline{8, 0, 7, "loop", 30};
	CHECK(
		Scalar(
			imagegraph_test::RunNode(
				"pc.fn_math", {}, {{"operation", EnumValue{0}}, {"value_1", 3.}, {"value_2", 4.}}
			)
		) == 7
	);
	CHECK(
		Scalar(
			imagegraph_test::RunNode(
				"pc.fn_math", {}, {{"operation", EnumValue{1}}, {"value_1", 3.}, {"value_2", 4.}}
			)
		) == -1
	);
	CHECK(Scalar(imagegraph_test::RunNode("pc.fn_math", {}, {{"value_1", 3.}, {"value_2", 4.}})) == 12);
	for (int64_t mode = 0; mode < 4; ++mode) {
		CHECK(
			Scalar(
				imagegraph_test::RunNode("pc.fn_smooth_step", {}, {{"value", .5}, {"type", EnumValue{mode}}})
			) == Catch::Approx(.5)
		);
		CHECK(
			Scalar(
				imagegraph_test::RunNode("pc.fn_ease", {}, {{"smooth", EnumValue{mode}}}, 0, 0, &timeline)
			) == 0
		);
		CHECK(
			Scalar(
				imagegraph_test::RunNode("pc.fn_ease", {}, {{"smooth", EnumValue{mode}}}, 1, 0, &timeline)
			) == 1
		);
	}
	CHECK(Scalar(imagegraph_test::RunNode("pc.fn_smooth_step", {}, {{"value", 2.}})) == -4);
	CHECK(
		Scalar(imagegraph_test::RunNode("pc.fn_wave_table", {}, {{"pattern", 0.}}, 1, 0, &timeline)) ==
		Catch::Approx(1)
	);
	CHECK(Scalar(imagegraph_test::RunNode("pc.fn_wave_table", {}, {{"pattern", 1.}}, 1, 0, &timeline)) == 1);
	CHECK(
		Scalar(imagegraph_test::RunNode("pc.fn_wave_table", {}, {{"pattern", 2.}}, 1, 0, &timeline)) ==
		Catch::Approx(.5)
	);
	CHECK(
		Scalar(imagegraph_test::RunNode("pc.fn_wave_table", {}, {{"pattern", .5}}, 1, 0, &timeline)) ==
		Catch::Approx(1)
	);
	CHECK(
		Scalar(
			imagegraph_test::RunNode(
				"pc.fn_wave_table", {}, {{"speed_control", EnumValue{1}}, {"period", 4.}}, 1, 0, &timeline
			)
		) == Catch::Approx(1)
	);
	CHECK(
		Scalar(
			imagegraph_test::RunNode(
				"pc.fn_wave_table",
				{},
				{{"attribute_wavetable", ArrayValue{ValueType::Integer, {int64_t{3}}}}},
				1,
				0,
				&timeline
			)
		) == Catch::Approx(.75)
	);
	CHECK(Scalar(imagegraph_test::RunNode("pc.wiggler", {}, {}, 0, 17, &timeline)) == .5);
	const auto wiggle =
		Scalar(imagegraph_test::RunNode("pc.wiggler", {}, {{"clip", int64_t{0}}}, 3, 17, &timeline));
	CHECK(wiggle >= 0);
	CHECK(wiggle <= 1);
	CHECK(
		Scalar(imagegraph_test::RunNode("pc.wiggler", {}, {{"clip", int64_t{0}}}, 3, 17, &timeline)) == wiggle
	);
	CHECK(
		imagegraph_test::RunNode(
			"pc.fn_wave_table", {}, {{"speed_control", EnumValue{1}}, {"period", 0.}}, 1, 0, &timeline
		)
			.Code == Status::UnsupportedExecution
	);

	Document doc;
	doc.FormatVersion = 9;
	doc.Timeline = timeline;
	doc.Nodes = {
		{"ease", "pc.fn_ease", "", {}, {}},
		{"math", "pc.fn_math", "", {}, {{"operation", EnumValue{0}}, {"value_2", double{2}}}}
	};
	doc.Links = {{"ease", "output", "math", "value_1"}};
	doc.Outputs = {{"out", "math", "output"}};
	Plan plan;
	Diagnostic diagnostic;
	EvaluatedValue result;
	EvaluationRequest request;
	request.Tick = 1;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 3);
	doc.Nodes = {{"wave", "pc.fn_wave_table", "", {}, {}}};
	doc.Links.clear();
	doc.Outputs = {{"out", "wave", "output"}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == Catch::Approx(1));
}
TEST_CASE(
	"source counter replay owns async increments reset and progress", "[imagegraph][source_animation]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Timeline = TimelineSettings{5, 0, 4, "loop", 30};
	doc.Nodes = {{"counter", "pc.counter", "", {}, {{"start", 10.}, {"speed", 2.}, {"async", true}}}};
	doc.Nodes[0].SourceAnimatedInputs = {"reset"};
	doc.Keyframes = {{"counter", "reset", 2, true}};
	doc.Outputs = {{"out", "counter", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	CapturedFeedbackHost host;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array expected{10., 12., 10., 12.};
	for (uint64_t tick = 0; tick < expected.size(); ++tick) {
		EvaluationRequest request;
		request.Tick = tick;
		const auto ok =
			host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
		INFO(diagnostic.Message);
		REQUIRE(ok);
		REQUIRE(host.Value("out"));
		CHECK(std::get<double>(std::get<EvaluatedValue>(host.Value("out")->Output).Data) == expected[tick]);
		REQUIRE(host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
		CHECK(std::get<double>(std::get<EvaluatedValue>(host.Value("out")->Output).Data) == expected[tick]);
	}
	EvaluationRequest bounded;
	bounded.Tick = 4;
	CHECK_FALSE(host.Prepare(doc, plan, 1, 1, bounded, diagnostic, 1, "out"));
	CHECK(std::get<double>(std::get<EvaluatedValue>(host.Value("out")->Output).Data) == 12);
	const TimelineSettings timeline{5, 0, 4, "loop", 30};
	CHECK(
		Scalar(
			imagegraph_test::RunNode("pc.counter", {}, {{"start", 10.}, {"speed", 2.}}, 2, 0, &timeline),
			"value"
		) == 14
	);
	CHECK(
		Scalar(
			imagegraph_test::RunNode(
				"pc.counter", {}, {{"mode", EnumValue{1}}, {"speed", 2.}}, 2, 0, &timeline
			),
			"value"
		) == 1
	);
}
TEST_CASE(
	"source value delay retains nested typed history and overflow defaults", "[imagegraph][source_animation]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	doc.Nodes = {
		{"source", "pc.equation", "", {}, {{"equation", std::string{"[Project.frame,\"é\",[4,6]]"}}}},
		{"fallback", "pc.string", "", {}, {{"text", std::string{"missing"}}}},
		{"delay", "pc.delay_value", "", {}, {{"frames", int64_t{1}}}}
	};
	doc.Links = {{"source", "result", "delay", "value"}, {"fallback", "text", "delay", "default"}};
	doc.Outputs = {{"out", "delay", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	CapturedFeedbackHost host;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	for (uint64_t tick = 0; tick < 3; ++tick) {
		EvaluationRequest request;
		request.Tick = tick;
		const auto ok =
			host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
		INFO(diagnostic.Message);
		REQUIRE(ok);
		REQUIRE(host.Value("out"));
		const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(host.Value("out")->Output).Data);
		REQUIRE(array.Items.size() == 3);
		CHECK(std::get<double>(std::get<ElementValue>(array.Items[0].Data)) == double(tick ? tick - 1 : 0));
		CHECK(std::get<std::string>(std::get<ElementValue>(array.Items[1].Data)) == "é");
		CHECK(std::get<std::vector<SourceArrayItem>>(array.Items[2].Data).size() == 2);
	}
	doc.Nodes[2].Values = {{"frames", int64_t{1}}, {"overflow", EnumValue{2}}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(host.Prepare(doc, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	const auto &fallbacks = std::get<ArrayValue>(std::get<EvaluatedValue>(host.Value("out")->Output).Data);
	REQUIRE(fallbacks.Items.size() == 3);
	for (const auto &item : fallbacks.Items)
		CHECK(std::get<std::string>(std::get<ElementValue>(item.Data)) == "missing");
	host.Clear();
	doc.Nodes[2].Values = {{"frames", int64_t{-1}}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(host.Prepare(doc, plan, 3, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	const auto &future = std::get<ArrayValue>(std::get<EvaluatedValue>(host.Value("out")->Output).Data);
	REQUIRE(future.Items.size() == 3);
	for (const auto &item : future.Items)
		CHECK(std::get<double>(std::get<ElementValue>(item.Data)) == 0);
}
TEST_CASE(
	"source surface temporal families replay input frames with exact source targets",
	"[imagegraph][source_animation]"
) {
	struct Example {
		const char *Type;
		std::vector<AuthoredValue> Values;
		std::array<uint8_t, 4> Red;
	};
	const std::array examples{
		Example{"pc.delay", {}, {30, 30, 50, 70}},
		Example{"pc.anim_loop", {{"loop_range", int64_t{2}}}, {30, 50, 30, 50}},
		Example{"pc.rate_remap", {{"framerate", 15.}}, {30, 30, 70, 70}},
		Example{"pc.revert", {}, {0, 0, 50, 30}},
		Example{"pc.stagger", {}, {30, 50, 70, 90}}
	};
	for (const auto &example : examples) {
		INFO(example.Type);
		auto doc = SurfaceGraph(example.Type, example.Values);
		Plan plan;
		Diagnostic diagnostic;
		StatefulEvaluationResult state;
		const auto compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		std::vector<RequestImageSource> sources{{"input", Solid(30)}};
		for (uint64_t tick = 0; tick < 4; ++tick) {
			sources[0].Data = Solid(uint8_t(30 + tick * 20));
			EvaluationRequest request;
			request.Tick = tick;
			request.ImageSources = sources;
			request.SurfaceReplay = &state.Surfaces;
			request.DataReplay = &state.Data;
			const auto evaluated = EvaluateStateful(doc, plan, "out", request, state, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(evaluated == Status::Ok);
			CHECK(std::get<Image>(state.Output).Pixels[0] == example.Red[tick]);
		}
		const auto retained = state;
		EvaluationRequest request;
		request.Tick = 4;
		request.ImageSources = sources;
		request.SurfaceReplay = &state.Surfaces;
		CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic, 1) == Status::LimitExceeded);
		CHECK(std::get<Image>(state.Output) == std::get<Image>(retained.Output));
		CHECK(state.Surfaces == retained.Surfaces);
		CHECK(state.Data == retained.Data);
	}
}
TEST_CASE(
	"source plot CPU profile handles bars graph pie paths maps and coverage requests",
	"[imagegraph][source_animation]"
) {
	const ArrayValue data{ValueType::Scalar, {double{2}, double{1}}};
	auto bars = imagegraph_test::RunNode(
		"pc.plot_linear",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"origin", Vector2{1, 7}},
		 {"origin_unit", EnumValue{0}},
		 {"scale", 2.},
		 {"scale_unit", EnumValue{0}},
		 {"bar_width", 2.},
		 {"spacing", 1.},
		 {"data", data},
		 {"base_color", Colour{255, 0, 0, 0}}}
	);
	INFO(bars.Message);
	REQUIRE(bars.Ok);
	CHECK(bars.Output().Width == 8);
	CHECK(bars.Output().Pixels[(4 * 8 + 1) * 4] == 255);
	CHECK(bars.Output().Pixels[(4 * 8 + 1) * 4 + 3] == 255);
	for (int64_t type = 0; type < 3; ++type) {
		auto plot = imagegraph_test::RunNode(
			"pc.plot_linear",
			{},
			{{"dimension", Vector2{16, 16}},
			 {"dimension_unit", EnumValue{0}},
			 {"data", data},
			 {"type", EnumValue{type}},
			 {"origin", Vector2{.5, .5}},
			 {"rounded_bar", true},
			 {"smooth", .5},
			 {"scale", .2},
			 {"loop", true},
			 {"radius", .4},
			 {"donut_radius", .5},
			 {"donut_separation", 1.}}
		);
		INFO(plot.Message);
		REQUIRE(plot.Ok);
		CHECK(plot.Output().Pixels.size() == 16 * 16 * 4);
		CHECK(std::any_of(plot.Output().Pixels.begin(), plot.Output().Pixels.end(), [](uint8_t x) {
			return x != 0;
		}));
	}
	Image green{1, 1, {0, 255, 0, 0}};
	Path2D path;
	path.Anchors = {{0, 0, 1, 7, 0, 0}, {0, 0, 7, 7, 0, 0}};
	auto mapped = imagegraph_test::RunNode(
		"pc.plot_linear",
		{{"color_over_sample_map", &green}},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"data", data},
		 {"origin", Vector2{1, 4}},
		 {"origin_unit", EnumValue{0}},
		 {"scale", 1.},
		 {"scale_unit", EnumValue{0}},
		 {"bar_width", 2.},
		 {"color_over_sample_mapped", true},
		 {"color_over_sample_map_range", Vector4{0, 0, 0, 0}},
		 {"trim_mode", EnumValue{1}},
		 {"window_size", int64_t{1}},
		 {"window_offset", .5},
		 {"flip_value", true}}
	);
	INFO(mapped.Message);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels[(4 * 8 + 1) * 4 + 1] == 255);
	CHECK(mapped.Output().Pixels[(4 * 8 + 1) * 4 + 3] == 255);
	auto pathPlot = imagegraph_test::RunNode(
		"pc.plot_linear",
		{},
		{{"dimension", Vector2{8, 8}},
		 {"dimension_unit", EnumValue{0}},
		 {"data", data},
		 {"path", path},
		 {"scale", 2.},
		 {"scale_unit", EnumValue{0}},
		 {"bar_width", 2.}}
	);
	INFO(pathPlot.Message);
	REQUIRE(pathPlot.Ok);
	CHECK(std::any_of(pathPlot.Output().Pixels.begin(), pathPlot.Output().Pixels.end(), [](uint8_t x) {
		return x != 0;
	}));

	Document doc;
	doc.FormatVersion = 9;
	doc.Project = ProjectSettings{};
	doc.Project->SurfaceWidth = 8;
	doc.Project->SurfaceHeight = 8;
	doc.Nodes = {
		{"plot", "pc.plot_linear", "", {}, {{"data", data}, {"type", EnumValue{0}}, {"background", true}}}
	};
	doc.Outputs = {{"out", "plot", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	Image result;
	EvaluationRequest request;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Width == 8);
	CHECK(result.Height == 8);
	const auto retained = result;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(Evaluate(doc, plan, "out", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(result == retained);
}

TEST_CASE(
	"delayed value history validates bounded owned trees before replay", "[imagegraph][source_animation]"
) {
	DataReplayEntry entry;
	entry.NodeId = "delay";
	entry.Initialized = true;
	StructValue object;
	object.Data.emplace();
	object.Data->Fields = {{"text", std::string{"retained"}}};
	entry.Values = {{0, object}, {1, ArrayValue{ValueType::Scalar, {double{4}, double{6}}}}};
	DataReplayState state{{entry}};
	Diagnostic diagnostic;
	REQUIRE(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	CHECK(RetainedDataReplayBytes(state) > sizeof(state) + sizeof(entry) + sizeof(DataReplayValueFrame) * 2);
	auto copied = state;
	std::get<StructValue>(copied.Entries[0].Values[0].Data).Data->Fields[0].second = std::string{"changed"};
	CHECK(
		std::get<std::string>(
			std::get<StructValue>(state.Entries[0].Values[0].Data).Data->Fields[0].second
		) == "retained"
	);
	CHECK(ValidateDataReplay(state, 1, diagnostic) == Status::LimitExceeded);
	copied.Entries[0].Values[1].Frame = 0;
	CHECK(ValidateDataReplay(copied, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	copied = state;
	std::get<ArrayValue>(copied.Entries[0].Values[1].Data).Elements[1] =
		std::numeric_limits<double>::quiet_NaN();
	CHECK(ValidateDataReplay(copied, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
}
