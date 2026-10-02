// Analytical expectations derived from pinned GML; no executable source parity is claimed.
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.curve_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document CurveGraph(Value progress = .25) {
		Document document;
		document.FormatVersion = 8;
		document.Nodes = {
			{"function", "pc.curve_function", "", {}, {}},
			{"sample",
			 "pc.anim_curve",
			 "",
			 {},
			 {{"progress", std::move(progress)}, {"minimum", 10.0}, {"maximum", 20.0}}}
		};
		document.Links = {{"function", "curve", "sample", "curve"}};
		document.Outputs = {{"result", "sample", "curve"}, {"control", "function", "curve"}};
		return document;
	}
	EvaluatedValue Evaluate(Document document, EvaluationRequest request = {}) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		const auto status = EvaluateValue(document, plan, "result", request, result, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
}
TEST_CASE(
	"curve function preserves all five source formulas and header controls", "[imagegraph][curve_nodes]"
) {
	const std::array<std::array<double, 5>, 4> expected{
		{{{.5, 1, .5, 0, .5}}, {{1, .5, 0, .5, 1}}, {{0, .25, .5, .75, 0}}, {{0, 1.0 / 3, 2.0 / 3, 1, 1}}}
	};
	const auto linear = RunNode("pc.curve_function", {});
	REQUIRE(linear.Ok);
	const auto &defaultCurve = std::get<Curve>(*linear.OutputValue("curve"));
	CHECK(defaultCurve.Header == std::array<double, 6>{0, 1, 0, 0, 1, 0});
	CHECK(defaultCurve.Anchors == std::vector<std::array<double, 6>>{{0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0}});
	for (int64_t mode = 1; mode <= 4; mode++) {
		CAPTURE(mode);
		const auto run = RunNode(
			"pc.curve_function",
			{},
			{{"type", EnumValue{mode}},
			 {"resolution", int64_t{4}},
			 {"step", int64_t{4}},
			 {"shift", .25},
			 {"scale", 2.0},
			 {"output_range", Vector2{2, 4}},
			 {"y_range", Vector2{123, 456}}}
		);
		REQUIRE(run.Ok);
		const auto &curve = std::get<Curve>(*run.OutputValue("curve"));
		CHECK(curve.Header == std::array<double, 6>{.25, 2, mode == 4 ? 1.0 : 0.0, 2, 4, 0});
		REQUIRE(curve.Anchors.size() == 5);
		for (size_t index = 0; index < 5; index++) {
			CHECK(curve.Anchors[index][2] == static_cast<double>(index) / 4);
			CHECK(curve.Anchors[index][3] == Catch::Approx(expected[mode - 1][index]).margin(1e-14));
			for (const size_t offset : {0, 1, 4, 5})
				CHECK(curve.Anchors[index][offset] == 0);
		}
	}
	const auto negative = RunNode(
		"pc.curve_function",
		{},
		{{"type", EnumValue{3}}, {"resolution", int64_t{4}}, {"phase", .25}, {"range", Vector2{2, 6}}}
	);
	REQUIRE(negative.Ok);
	const auto &curve = std::get<Curve>(*negative.OutputValue("curve"));
	CHECK(curve.Anchors.front()[3] == 5);
	CHECK(curve.Anchors[1][3] == 2);
	const auto frequency = RunNode(
		"pc.curve_function", {}, {{"type", EnumValue{1}}, {"frequency", 1.5}, {"resolution", int64_t{4}}}
	);
	REQUIRE(frequency.Ok);
	const auto &frequencyCurve = std::get<Curve>(*frequency.OutputValue("curve"));
	REQUIRE(frequencyCurve.Anchors.size() == 5);
	CHECK(frequencyCurve.Anchors[0][3] == Catch::Approx(.5).margin(1e-14));
	CHECK(frequencyCurve.Anchors[1][3] == Catch::Approx((1 + std::sqrt(.5)) / 2).margin(1e-14));
	CHECK(frequencyCurve.Anchors[2][3] == Catch::Approx(0).margin(1e-14));
	CHECK(frequencyCurve.Anchors[3][3] == Catch::Approx((1 + std::sqrt(.5)) / 2).margin(1e-14));
	CHECK(frequencyCurve.Anchors[4][3] == Catch::Approx(.5).margin(1e-14));
	const auto frequencyZigzag = RunNode(
		"pc.curve_function", {}, {{"type", EnumValue{2}}, {"frequency", 1.5}, {"resolution", int64_t{4}}}
	);
	REQUIRE(frequencyZigzag.Ok);
	const auto &frequencyZigzagCurve = std::get<Curve>(*frequencyZigzag.OutputValue("curve"));
	REQUIRE(frequencyZigzagCurve.Anchors.size() == 5);
	CHECK(frequencyZigzagCurve.Anchors[0][3] == Catch::Approx(1).margin(1e-14));
	CHECK(frequencyZigzagCurve.Anchors[1][3] == Catch::Approx(.25).margin(1e-14));
	CHECK(frequencyZigzagCurve.Anchors[2][3] == Catch::Approx(.5).margin(1e-14));
	CHECK(frequencyZigzagCurve.Anchors[3][3] == Catch::Approx(.75).margin(1e-14));
	CHECK(frequencyZigzagCurve.Anchors[4][3] == Catch::Approx(0).margin(1e-14));
}
TEST_CASE(
	"compiled curve workflow preserves scalar collapse arrays and serialized links",
	"[imagegraph][curve_nodes]"
) {
	CHECK(std::get<double>(Evaluate(CurveGraph()).Data) == 12.5);
	const ArrayValue progress{ValueType::Scalar, {0.0, .25, .5, 1.0}};
	Document document = CurveGraph(progress);
	CHECK(Evaluate(document).Data == Value{ArrayValue{ValueType::Scalar, {10.0, 12.5, 15.0, 20.0}}});
	Diagnostic diagnostic;
	Document roundtrip;
	REQUIRE(Read(Write(document), roundtrip, diagnostic) == Status::Ok);
	CHECK(Evaluate(roundtrip).Data == Evaluate(document).Data);
	CHECK(std::get<double>(Evaluate(CurveGraph(ArrayValue{ValueType::Scalar, {.5}})).Data) == 15);
	ArrayValue nested{ValueType::Scalar, {}, {{0.0, .5}, {.25, 1.0}}};
	Document nestedDocument = CurveGraph(nested);
	Plan nestedPlan;
	CHECK(Compile(nestedDocument, nestedPlan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.Message == "authored property has the wrong value type");
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Port == "progress");

	document = CurveGraph(progress);
	document.Nodes[0].Values = {{"type", EnumValue{4}}, {"step", int64_t{4}}};
	const auto result = Evaluate(document);
	const auto &values = std::get<ArrayValue>(result.Data).Elements;
	REQUIRE(values.size() == 4);
	CHECK(std::get<double>(values[0]) == 10);
	CHECK(std::get<double>(values[1]) == 10);
	CHECK(std::get<double>(values[2]) == Catch::Approx(10 + 10.0 / 3));
	CHECK(std::get<double>(values[3]) == 20);
}
TEST_CASE(
	"curve sampling uses source headers timeline subframes and presentation independent output",
	"[imagegraph][curve_nodes]"
) {
	Document document = CurveGraph();
	document.Nodes[0].Values = {{"shift", .25}, {"scale", 2.0}, {"output_range", Vector2{2, 4}}};
	document.Nodes[1].Values = {
		{"progress", 1.0}, {"minimum", 0.0}, {"maximum", 1.0}, {"display_type", EnumValue{1}}
	};
	CHECK(std::get<double>(Evaluate(document).Data) == 2.5);
	document = CurveGraph();
	document.Timeline = TimelineSettings{5, 1, 3, "loop", 30};
	document.Nodes[1].Values = {{"animated", true}};
	EvaluationRequest request;
	request.Tick = 1;
	request.Subframe = .5;
	CHECK(std::get<double>(Evaluate(document, request).Data) == .375);
	request.Tick = 4;
	request.Subframe = 0;
	CHECK(std::get<double>(Evaluate(document, request).Data) == 1);
	document = CurveGraph();
	document.Nodes.push_back({"offset", "pc.number", "", {}, {{"value", .25}}});
	document.Links.push_back({"offset", "number", "function", "shift"});
	CHECK(std::get<double>(Evaluate(document).Data) == 10);
}
TEST_CASE(
	"curve generators reject invalid denominators and limits before publishing", "[imagegraph][curve_nodes]"
) {
	for (const int64_t mode : {1, 2, 3}) {
		const auto invalid = RunNode("pc.curve_function", {}, {{"type", EnumValue{mode}}});
		CHECK_FALSE(invalid.Ok);
		CHECK(invalid.Code == Status::InvalidValue);
		CHECK(invalid.Port == "resolution");
		CHECK(invalid.Values.empty());
	}
	for (const int64_t step : {0, 1}) {
		const auto invalid = RunNode("pc.curve_function", {}, {{"type", EnumValue{4}}, {"step", step}});
		CHECK_FALSE(invalid.Ok);
		CHECK(invalid.Port == "step");
	}
	const auto boundary =
		RunNode("pc.curve_function", {}, {{"type", EnumValue{3}}, {"resolution", int64_t{255}}});
	REQUIRE(boundary.Ok);
	CHECK(std::get<Curve>(*boundary.OutputValue("curve")).Anchors.size() == Limits::MaximumCurveAnchors);
	const auto overflow =
		RunNode("pc.curve_function", {}, {{"type", EnumValue{3}}, {"resolution", int64_t{256}}});
	CHECK(overflow.Code == Status::LimitExceeded);
	CHECK(overflow.Values.empty());
	for (const double scale : {0.0, std::numeric_limits<double>::infinity()}) {
		const auto invalid = RunNode("pc.curve_function", {}, {{"scale", scale}});
		CHECK_FALSE(invalid.Ok);
		CHECK(invalid.Port == "scale");
	}
	const auto phase = RunNode(
		"pc.curve_function",
		{},
		{{"type", EnumValue{1}},
		 {"resolution", int64_t{4}},
		 {"phase", std::numeric_limits<double>::quiet_NaN()}}
	);
	CHECK_FALSE(phase.Ok);
	CHECK(phase.Port == "phase");
	const auto hugeFrequency = RunNode(
		"pc.curve_function",
		{},
		{{"type", EnumValue{1}},
		 {"resolution", int64_t{4}},
		 {"frequency", std::numeric_limits<double>::max()}}
	);
	CHECK_FALSE(hugeFrequency.Ok);
	CHECK(hugeFrequency.Port == "frequency");
	CHECK(hugeFrequency.Values.empty());
	Curve invalidCurve{{0, 0, 0, 0, 1, 0}, {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0}}};
	const auto invalidSample = RunNode("pc.anim_curve", {}, {{"curve", invalidCurve}});
	CHECK_FALSE(invalidSample.Ok);
	CHECK(invalidSample.Port == "curve");
	const TimelineSettings single{1, 0, 0, "loop", 30};
	const auto animated = RunNode("pc.anim_curve", {}, {{"animated", true}}, 0, 0, &single);
	CHECK_FALSE(animated.Ok);
	CHECK(animated.Port == "animated");
	const auto nonfinite =
		RunNode("pc.anim_curve", {}, {{"progress", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Port == "progress");
	Node node{"bounded", "pc.curve_function", "", {}, {}};
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	const uint64_t outputSlots =
		entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								 (sizeof(std::pair<std::string, ImageArray>) +
								  sizeof(std::pair<std::string_view, SourceSocketDomain>)));
	const uint64_t outputName =
		std::max<uint64_t>(std::string_view("curve").size(), std::string{}.capacity());
	const uint64_t curveBytes = outputSlots + outputName + 2 * sizeof(std::array<double, 6>);
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = curveBytes - 1;
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	REQUIRE(detail::FindExecutor(node.Type));
	CHECK_FALSE(detail::FindExecutor(node.Type)(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
	detail::NodeContext exact(node, *entry, request);
	exact.Values = context.Values;
	exact.ByteBudget = curveBytes;
	REQUIRE(detail::FindExecutor(node.Type)(exact));
	REQUIRE(exact.OutputValues.size() == 1);
	CHECK(detail::RetainedPayloadBytes(exact.OutputValues.front().Data) == 2 * sizeof(std::array<double, 6>));

	Node sampleNode{"sample", "pc.anim_curve", "", {}, {}};
	const auto *sampleEntry = FindCatalogueEntry(sampleNode.Type);
	REQUIRE(sampleEntry);
	const uint64_t sampleSlots =
		sampleEntry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
									   (sizeof(std::pair<std::string, ImageArray>) +
										sizeof(std::pair<std::string_view, SourceSocketDomain>)));
	const uint64_t sampleBytes = sampleSlots + outputName;
	detail::NodeContext shortSample(sampleNode, *sampleEntry, request);
	for (const auto &input : sampleEntry->Inputs)
		if (const auto value = CatalogueDefault(input)) shortSample.Values.emplace_back(input.Id, *value);
	shortSample.ByteBudget = sampleBytes - 1;
	CHECK_FALSE(detail::FindExecutor(sampleNode.Type)(shortSample));
	CHECK(shortSample.FailureCode == Status::LimitExceeded);
	CHECK(shortSample.OutputValues.empty());
	detail::NodeContext exactSample(sampleNode, *sampleEntry, request);
	exactSample.Values = shortSample.Values;
	exactSample.ByteBudget = sampleBytes;
	REQUIRE(detail::FindExecutor(sampleNode.Type)(exactSample));
	REQUIRE(exactSample.OutputValues.size() == 1);
	CHECK(detail::RetainedPayloadBytes(exactSample.OutputValues.front().Data) == 0);
}
