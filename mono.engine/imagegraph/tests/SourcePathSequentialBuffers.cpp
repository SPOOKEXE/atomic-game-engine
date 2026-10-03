#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
TEST_SUITE_ID("engine.imagegraph.source_path_sequential_buffers")
using namespace engine::imagegraph;
namespace {
	Document WeightedExtendSamples() {
		Path2D line;
		line.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		line.Weights = {{0, 3}, {100, 3}};
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"extend", "pc.path_extends", "", {}, {{"path", line}, {"side", EnumValue{1}}, {"length", 5.}}},
			{"ratios", "pc.array", "", {}, {{"type", EnumValue{0}}}},
			{"a", "pc.path_sample", "", {}, {{"type", EnumValue{2}}}},
			{"b", "pc.path_sample", "", {}, {{"type", EnumValue{2}}, {"ratio", 14. / 15}}}
		};
		d.Nodes[1].DynamicInputs = {
			{"input_0", ValueType::Scalar, Value{.5}}, {"input_1", ValueType::Scalar, Value{.9}}
		};
		d.Links = {
			{"extend", "path", "a", "path"},
			{"extend", "path", "b", "path"},
			{"ratios", "array", "a", "ratio"}
		};
		d.Outputs = {{"a", "a", "weight"}, {"b", "b", "weight"}};
		return d;
	}
}
TEST_CASE(
	"Sequential sample buffers belong to their durable node across rows and ticks", "[source_path_sequential]"
) {
	auto d = WeightedExtendSamples();
	Plan plan;
	Diagnostic error;
	StatefulOutputEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &result.Data;
	const std::array<std::string, 2> outputs{"a", "b"};
	const auto compiled = Compile(d, plan, error);
	INFO(error.Message << " " << error.NodeId << ":" << error.Port);
	REQUIRE(compiled == Status::Ok);
	REQUIRE(EvaluateStatefulOutputs(d, plan, outputs, request, result, error) == Status::Ok);
	REQUIRE(result.Outputs.size() == 2);
	const auto checkWeights = [&](double a, double b) {
		REQUIRE(result.Outputs[0].Id == "a");
		REQUIRE(result.Outputs[1].Id == "b");
		const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data);
		REQUIRE(array.ElementType == ValueType::Scalar);
		REQUIRE(array.Elements.size() == 2);
		CHECK(std::get<double>(array.Elements[0]) == a);
		CHECK(std::get<double>(array.Elements[1]) == a);
		CHECK(std::get<double>(std::get<EvaluatedValue>(result.Outputs[1].Output).Data) == b);
	};
	checkWeights(3, 1);
	const auto prior = result.Data;
	const auto priorSnapshot = prior;
	request.DataReplay = &prior;
	request.Tick = 1;
	d.Nodes[1].DynamicInputs[0].Default = Value{.8};
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	REQUIRE(EvaluateStatefulOutputs(d, plan, outputs, request, result, error) == Status::Ok);
	checkWeights(3, 1);
	CHECK(prior == priorSnapshot);
	request.DataReplay = nullptr;
	request.Tick = 0;
	REQUIRE(EvaluateStatefulOutputs(d, plan, outputs, request, result, error) == Status::Ok);
	checkWeights(1, 1);
}

TEST_CASE(
	"Sequential owner buffers survive host seek reconstruction and reset",
	"[source_path_sequential][feedback][stateful_replay]"
) {
	Document d;
	d.FormatVersion = 9;
	SECTION("weighted inside sample initializes the later extension buffer") {
		Path2D line;
		line.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		line.Weights = {{0, 3}, {100, 3}};
		d.Nodes = {
			{"extend", "pc.path_extends", "", {}, {{"path", line}, {"side", EnumValue{1}}, {"length", 5.}}},
			{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
		};
		d.Links = {{"extend", "path", "sample", "path"}};
		d.Keyframes = {{"sample", "ratio", 0, .5, "step"}, {"sample", "ratio", 1, .9, "step"}};
		d.Outputs = {{"out", "sample", "weight"}};
	}
	SECTION("raw spatial sampling initializes the transformed caller class") {
		PathValue3D raw;
		raw.Data.emplace().Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0, 0, 0, 0}, 0}};
		auto transformed = raw;
		PathTransform3D transform;
		transform.Position.Z = 3;
		transformed.Data->Transforms.push_back(transform);
		d.Junctions = {
			{"raw", "", ValueType::Path3D, raw}, {"transformed", "", ValueType::Path3D, transformed}
		};
		d.Nodes = {
			{"pool", "pc.array", "", {}, {{"type", EnumValue{0}}}},
			{"pick", "pc.array_get", "", {}, {{"index", int64_t{0}}}},
			{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
		};
		d.Nodes[0].DynamicInputs = {
			{"input_0", ValueType::Any, std::nullopt}, {"input_1", ValueType::Any, std::nullopt}
		};
		d.Links = {
			{"raw", "value", "pool", "input_0"},
			{"transformed", "value", "pool", "input_1"},
			{"pool", "array", "pick", "array"},
			{"pick", "value", "sample", "path"}
		};
		d.Keyframes = {{"pick", "index", 0, int64_t{0}, "step"}, {"pick", "index", 1, int64_t{1}, "step"}};
		d.Outputs = {{"out", "sample", "position"}};
	}
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(d, plan, error) == Status::Ok);
	const std::array<std::string, 1> selected{"out"};
	const auto cone = AnalyzeStatefulTemporalCone(d, plan, selected);
	REQUIRE(cone.Valid);
	CHECK(cone.FirstFrameData);
	CapturedFeedbackHost playback, seek;
	const auto prepare = [&](CapturedFeedbackHost &host, uint64_t tick) {
		EvaluationRequest request;
		request.Tick = tick;
		const auto okay = host.Prepare(d, plan, 1, 1, request, error, Limits::MaximumEvaluationBytes, "out");
		INFO(error.Message);
		REQUIRE(okay);
		REQUIRE(request.DataReplay);
		REQUIRE(host.Value("out"));
		return std::pair{std::get<EvaluatedValue>(host.Value("out")->Output).Data, *request.DataReplay};
	};
	const auto initial = prepare(playback, 0);
	const auto one = prepare(playback, 1);
	const auto two = prepare(playback, 2);
	const auto three = prepare(playback, 3);
	if (d.Outputs[0].Port == "weight") {
		CHECK(std::get<double>(initial.first) == 3);
		CHECK(std::get<double>(three.first) == 3);
	} else {
		CHECK(std::get<Vector3>(initial.first) == Vector3{5, 0, 0});
		CHECK(std::get<Vector3>(three.first) == Vector3{5, 0, 3});
	}
	const auto freshSeek = prepare(seek, 3);
	CHECK(freshSeek == three);
	CHECK(prepare(playback, 1) == one);
	CHECK(prepare(playback, 2) == two);
	CHECK(prepare(playback, 3) == three);
	CHECK(prepare(playback, 3) == three);
	const auto beforeRefusal = prepare(playback, 3);
	EvaluationRequest failed;
	failed.Tick = 4;
	CHECK_FALSE(playback.Prepare(d, plan, 1, 1, failed, error, 1, "out"));
	CHECK(prepare(playback, 3) == beforeRefusal);
	playback.Clear();
	CHECK(prepare(playback, 0) == initial);
	CHECK(prepare(playback, 3) == three);
}
