#include "EvaluationAllocator.hpp"
#include "SourceGradientValue.hpp"
#include "TimelineDrivers.hpp"
#include "TimelineOverrides.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_gradient_timeline")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace {
	Gradient First(uint8_t mode = 0) {
		return {mode, {{0, {0, 0, 0, 10}}, {1, {100, 100, 100, 110}}}};
	}
	Gradient Last() {
		return {1, {{0, {200, 200, 200, 210}}, {.5, {100, 100, 100, 110}}, {1, {0, 0, 0, 10}}}};
	}
} // namespace
TEST_CASE(
	"Persisted gradient interpolation follows source key count, time and "
	"RGBA merge",
	"[imagegraph][source_gradient_timeline]"
) {
	Gradient result;
	REQUIRE(LerpSourceGradient(First(), Last(), .5, result) == Status::Ok);
	CHECK(result.Mode == 0);
	REQUIRE(result.Keys.size() == 3);
	CHECK(result.Keys[0] == GradientKey{0, {100, 100, 100, 110}});
	CHECK(result.Keys[1] == GradientKey{.25, {112, 112, 112, 122}});
	CHECK(result.Keys[2] == GradientKey{1, {50, 50, 50, 60}});
	REQUIRE(LerpSourceGradient(First(), Last(), 1, result) == Status::Ok);
	CHECK(result.Mode == 0);
	CHECK(result.Keys == Last().Keys);
	Gradient one{0, {{.75, {10, 20, 30, 40}}}}, other{1, {{.25, {100, 110, 120, 130}}}};
	REQUIRE(LerpSourceGradient(one, other, .5, result) == Status::Ok);
	CHECK(result.Keys == std::vector<GradientKey>{{0, {55, 65, 75, 85}}});
	REQUIRE(LerpSourceGradient(Gradient{0, {}}, one, .5, result) == Status::Ok);
	CHECK(result.Keys == std::vector<GradientKey>{{0, {5, 10, 15, 20}}});
	REQUIRE(LerpSourceGradient(First(), Last(), -3, result) == Status::Ok);
	CHECK(result.Keys.empty());
	const auto previous = result;
	CHECK(LerpSourceGradient(First(), Last(), 200, result) == Status::LimitExceeded);
	CHECK(result == previous);
	CHECK(LerpSourceGradient(Gradient{0, {}}, Last(), 1, result) == Status::UnsupportedExecution);
	CHECK(result == previous);
	CHECK(
		LerpSourceGradient(First(), Last(), .5, result, 3 * sizeof(GradientKey) - 1) == Status::LimitExceeded
	);
	CHECK(result == previous);
	CHECK(
		LerpSourceGradient(First(), Last(), std::numeric_limits<double>::infinity(), result) ==
		Status::InvalidValue
	);
	CHECK(result == previous);
	result = one;
	const auto oldOne = result;
	CHECK(LerpSourceGradient(one, other, .5, result, sizeof(GradientKey)) == Status::LimitExceeded);
	CHECK(result == oldOne);
	CHECK(LerpSourceGradient(one, other, 1e308, result) == Status::InvalidValue);
	CHECK(result == oldOne);
	REQUIRE(LerpSourceGradient(First(), Last(), 2, result) == Status::Ok);
	REQUIRE(result.Keys.size() == 4);
	CHECK(result.Keys[0].Time == 0);
	CHECK(result.Keys[1].Time == 0);
	CHECK(result.Keys[2].Time == 1);
	CHECK(result.Keys[3].Time == 1);
}
TEST_CASE(
	"Persisted gradient interpolation samples RGB and step modes with source alpha",
	"[imagegraph][source_gradient_timeline]"
) {
	Gradient from{0, {{0, {200, 50, 70, 10}}, {1, {20, 180, 210, 110}}}}, result;
	REQUIRE(LerpSourceGradient(from, Last(), .5, result) == Status::Ok);
	CHECK(
		result.Keys == std::vector<GradientKey>{
						   {0, {200, 125, 135, 110}}, {.25, {178, 141, 152, 122}}, {1, {10, 90, 105, 60}}
					   }
	);
	from.Mode = 1;
	REQUIRE(LerpSourceGradient(from, Last(), .5, result) == Status::Ok);
	CHECK(result.Mode == 1);
	CHECK(
		result.Keys == std::vector<GradientKey>{
						   {0, {200, 125, 135, 110}}, {.25, {200, 125, 135, 110}}, {1, {10, 90, 105, 60}}
					   }
	);
	// NodeGradient.cpp independently checks HSV, inverse HSV, sRGB, Oklab and CMYK sampler goldens.
}
TEST_CASE(
	"Gradient timeline keeps exact signed keys, source cuts and driver "
	"refusal atomic",
	"[imagegraph][source_gradient_timeline]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"g", "pc.gradient_out", "", {}, {{"gradient", First()}, {"sample", .25}}}};
	document.Outputs = {{"out", "g", "gradient"}};
	document.Tracks = {{"g", "gradient", "hold", -1}};
	document.Keyframes = {
		{"g", "gradient", 0, First(), "source", KeyframeEase{}},
		{"g", "gradient", 4, Last(), "source", KeyframeEase{}},
		{"g", "gradient", 8, First(1), "source", KeyframeEase{}}
	};
	REQUIRE(SetFrameTime(document.Keyframes.front(), {2, .5, true}));
	Plan plan;
	Diagnostic diagnostic;
	const auto compileStatus = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compileStatus == Status::Ok);
	EvaluationRequest request;
	EvaluatedValue result;
	for (const FrameTime frame : {FrameTime{2, .5, true}, FrameTime{4, 0, false}, FrameTime{8, 0, false}}) {
		REQUIRE(SetFrameTime(request, frame));
		REQUIRE(EvaluateValue(document, plan, "out", request, result, diagnostic) == Status::Ok);
		const Gradient &gradient = std::get<Gradient>(result.Data);
		if (frame.NegativeFrame)
			CHECK(gradient == First());
		else if (frame.Tick == 4)
			CHECK(gradient == Last());
		else
			CHECK(gradient == First(1));
	}
	// Fractional key-map interpolation remains a separate source coercion boundary.
	REQUIRE(SetFrameTime(document.Keyframes.front(), {2, 0, true}));
	// A source cut chooses a raw key before its numeric driver runs.
	document.Keyframes[0].Ease->OutType = "cut";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(SetFrameTime(request, {1, 0, false}));
	const auto cutStatus = EvaluateValue(document, plan, "out", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(cutStatus == Status::Ok);
	CHECK(std::get<Gradient>(result.Data) == Last());
	document.Keyframes[1].Ease->InType = "cut";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Gradient>(result.Data) == First());
	document.Keyframes[1].Ease->InType = "linear";
	document.Keyframes[0].SourceDriver = KeyframeLinearDriver{1};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	TimelineOverrides overrides;
	const std::array<uint8_t, 1> needed{1};
	REQUIRE(ResolveTimelineOverrides(document, needed, request, budget, overrides, diagnostic) == Status::Ok);
	CHECK(std::get<Gradient>(overrides.Find(0, document.Nodes[0]).Values[0].Data) == Last());
	const uint64_t retainedBytes = budget.Used();
	document.Keyframes[0].Ease->OutType = "linear";
	CHECK(
		ResolveTimelineOverrides(document, needed, request, budget, overrides, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(std::get<Gradient>(overrides.Find(0, document.Nodes[0]).Values[0].Data) == Last());
	CHECK(budget.Used() == retainedBytes);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto oldOutput = result;
	CHECK(EvaluateValue(document, plan, "out", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(result == oldOutput);
	document.Keyframes.back().SourceDriver = KeyframeBounceDriver{3, .5, 2};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(SetFrameTime(request, {8, 0, false}));
	REQUIRE(EvaluateValue(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Gradient>(result.Data) == First(1));

	const KeyframeSourceDriver bounce = KeyframeBounceDriver{3, .5, 2};
	Value output = int64_t(91);
	CHECK(ApplySourceDriver(&bounce, First(), Last(), 1, 1, 8, false, -1, output) == Status::Ok);
	CHECK(std::get<Gradient>(output) == First());
	const Value retained = output;
	CHECK(
		ApplySourceDriver(&bounce, First(), Last(), .5, .5, 2, true, -1, output) ==
		Status::UnsupportedExecution
	);
	CHECK(output == retained);
}
