#include "NodeHarness.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_routing")
using namespace engine::imagegraph;
namespace {
	Document Routed() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"left", "pc.equation", "", {}, {{"equation", std::string{"[1,\"left\",[3,4]]"}}}},
			{"right", "pc.string", "", {}, {{"text", std::string{"right"}}}},
			{"choose", "pc.condition", "", {}, {{"boolean", true}}}
		};
		doc.Links = {{"left", "result", "choose", "true"}, {"right", "text", "choose", "false"}};
		doc.Outputs = {{"result", "choose", "result"}, {"bool", "choose", "bool"}};
		return doc;
	}
} // namespace
TEST_CASE(
	"source condition selects complete typed branches in three "
	"comparison modes",
	"[imagegraph][source_routing]"
) {
	auto doc = Routed();
	Plan plan;
	Diagnostic diagnostic;
	EvaluatedValue value;
	EvaluationRequest request;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "result", request, value, diagnostic) == Status::Ok);
	const auto &selected = std::get<ArrayValue>(value.Data);
	REQUIRE(selected.Items.size() == 3);
	CHECK(std::get<std::string>(std::get<ElementValue>(selected.Items[1].Data)) == "left");
	CHECK(std::get<std::vector<SourceArrayItem>>(selected.Items[2].Data).size() == 2);
	doc.Nodes[2].Values = {{"boolean", false}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "result", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "right");
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(restored, plan, "result", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "right");
	const std::array<bool, 6> expected{false, true, true, true, false, false};
	for (int64_t comparison = 0; comparison < 6; ++comparison) {
		doc.Nodes[2].Values = {
			{"eval_mode", EnumValue{1}},
			{"condition", EnumValue{comparison}},
			{"check_value", 2.},
			{"compare_to", 3.}
		};
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(doc, plan, "bool", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<bool>(value.Data) == expected[size_t(comparison)]);
	}
	doc.Nodes[2].Values = {
		{"eval_mode", EnumValue{2}}, {"text_1", std::string{"é"}}, {"text_2", std::string{"é"}}
	};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "bool", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<bool>(value.Data));
}
TEST_CASE(
	"source transform and rotation descriptors preserve their five "
	"number arrays",
	"[imagegraph][source_routing]"
) {
	Document doc;
	doc.FormatVersion = 9;
	Plan plan;
	Diagnostic diagnostic;
	EvaluatedValue value;
	EvaluationRequest request;
	doc.Nodes = {
		{"tuple",
		 "pc.transform_array",
		 "",
		 {},
		 {{"postion", Vector2{2, 3}}, {"rotation", 42.}, {"scale", Vector2{4, 9}}}}
	};
	doc.Outputs = {{"out", "tuple", "transform"}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data).Elements == std::vector<ElementValue>{2., 3., 42., 4., 4.});
	doc.Nodes[0].Values[1].Data = ArrayValue{ValueType::Scalar, {10., 20.}};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(doc, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(
		std::get<ArrayValue>(value.Data).Nested ==
		std::vector<std::vector<ElementValue>>{{2., 3., 10., 4., 4.}, {2., 3., 20., 4., 4.}}
	);
	for (int64_t mode = 0; mode < 5; ++mode) {
		doc.Nodes = {
			{"tuple",
			 "pc.rotation_random_data",
			 "",
			 {},
			 {{"type", EnumValue{mode}},
			  {"range_start", 10.},
			  {"range_end", 20.},
			  {"range_2_start", 30.},
			  {"range_2_end", 40.}}}
		};
		doc.Outputs = {{"out", "tuple", "rotation_random"}};
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(doc, plan, "out", request, value, diagnostic) == Status::Ok);
		const std::vector<ElementValue> expected =
			mode == 0 ? std::vector<ElementValue>{0., 10., 10., 10., 10.}
					  : std::vector<ElementValue>{double(mode - 1), 10., 20., 30., 40.};
		CHECK(std::get<ArrayValue>(value.Data).Elements == expected);
	}
}
TEST_CASE(
	"source value cache keeps absolute frame gaps and owned nested values", "[imagegraph][source_routing]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Timeline = TimelineSettings{5, 0, 4, "loop", 30};
	doc.Nodes = {
		{"source", "pc.equation", "", {}, {{"equation", std::string{"[Project.frame,\"text\",[4,6]]"}}}},
		{"cache", "pc.cache_value_array", "", {}, {{"start_frame", int64_t{2}}, {"stop_frame", int64_t{3}}}}
	};
	doc.Links = {{"source", "result", "cache", "value"}};
	doc.Outputs = {{"out", "cache", "cache_array"}};
	Plan plan;
	Diagnostic diagnostic;
	CapturedFeedbackHost host;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	const std::array<std::string, 1> outputs{"out"};
	CHECK(AnalyzeStatefulTemporalCone(doc, plan, outputs).DataProcessors == 1);
	for (uint64_t tick = 0; tick < 5; ++tick) {
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
		const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(host.Value("out")->Output).Data);
		CHECK(
			array.Items.size() + array.Elements.size() + array.Nested.size() == (tick == 0	 ? 0
																				 : tick == 1 ? 2
																							 : 3)
		);
		REQUIRE(host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	}
	EvaluationRequest request;
	request.Tick = 4;
	const auto before = *host.Value("out");
	const auto &cached = std::get<ArrayValue>(std::get<EvaluatedValue>(before.Output).Data);
	REQUIRE(cached.Items.size() == 3);
	CHECK(std::get<double>(std::get<ElementValue>(cached.Items[0].Data)) == 0);
	for (size_t index = 1; index < 3; ++index) {
		const auto &frame = std::get<std::vector<SourceArrayItem>>(cached.Items[index].Data);
		REQUIRE(frame.size() == 3);
		CHECK(std::get<double>(std::get<ElementValue>(frame[0].Data)) == double(index));
		CHECK(std::get<std::string>(std::get<ElementValue>(frame[1].Data)) == "text");
		CHECK(std::get<std::vector<SourceArrayItem>>(frame[2].Data).size() == 2);
	}
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost replay;
	REQUIRE(replay.Prepare(restored, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	const auto &unvisited = std::get<ArrayValue>(std::get<EvaluatedValue>(replay.Value("out")->Output).Data);
	CHECK(unvisited.Items.empty());
	CHECK(unvisited.Elements.empty());
	CHECK(unvisited.Nested.empty());
	for (uint64_t tick = 0; tick < 5; ++tick) {
		request.Tick = tick;
		REQUIRE(
			replay.Prepare(restored, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
		);
	}
	CHECK(
		std::get<EvaluatedValue>(replay.Value("out")->Output).Data ==
		std::get<EvaluatedValue>(before.Output).Data
	);
	CHECK_FALSE(host.Prepare(doc, plan, 1, 1, request, diagnostic, 1, "out"));
	CHECK(
		std::get<EvaluatedValue>(host.Value("out")->Output).Data ==
		std::get<EvaluatedValue>(before.Output).Data
	);
}
TEST_CASE(
	"source condition forwards an owned selected surface without "
	"precision conversion",
	"[imagegraph][source_routing]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
		{"choose", "pc.condition", "", {}, {{"boolean", true}}}
	};
	doc.Links = {{"input", "image", "choose", "true"}};
	doc.Outputs = {{"out", "choose", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image source{1, 1, {11, 22, 33, 44}};
	source.Hash = SurfaceHash(source);
	std::vector<RequestImageSource> inputs{{"input", source}};
	EvaluationRequest request;
	request.ImageSources = inputs;
	Image output;
	REQUIRE(Evaluate(doc, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == source.Pixels);
	CHECK(output.Format == source.Format);
	inputs[0].Data.Pixels[0] = 90;
	CHECK(output.Pixels[0] == 11);
}
TEST_CASE(
	"source value cache rejects hostile clocks and history without publishing", "[imagegraph][source_routing]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Timeline = TimelineSettings{5, 0, 4, "loop", 30};
	doc.Nodes = {
		{"source", "pc.number_simple", "", {}, {{"value", 7.}}}, {"cache", "pc.cache_value_array", "", {}, {}}
	};
	doc.Links = {{"source", "number", "cache", "value"}};
	doc.Outputs = {{"out", "cache", "cache_array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult state;
	EvaluationRequest request;
	request.DataReplay = &state.Data;
	REQUIRE(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::Ok);
	const auto before = std::get<EvaluatedValue>(state.Output).Data;
	const auto history = state.Data;
	request.Tick = std::numeric_limits<uint64_t>::max();
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::LimitExceeded);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == before);
	request.Tick = 1;
	request.Subframe = .5;
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::UnsupportedExecution);
	CHECK(state.Data == history);
	request.Subframe = 0;
	request.NegativeFrame = true;
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == before);
	request.NegativeFrame = false;
	request.Tick = 4;
	request.Subframe = .5;
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == before);
	request.Tick = 0;
	request.NegativeFrame = true;
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == before);
	request.Subframe = 0;
	DataReplayState hostile = history;
	REQUIRE(hostile.Entries.size() == 1);
	REQUIRE(hostile.Entries[0].Values.size() == 1);
	hostile.Entries[0].Values[0].Frame = std::numeric_limits<uint64_t>::max();
	request.Tick = 0;
	request.NegativeFrame = false;
	request.DataReplay = &hostile;
	CHECK(EvaluateStateful(doc, plan, "out", request, state, diagnostic) == Status::InvalidValue);
	CHECK(state.Data == history);
	CHECK(std::get<EvaluatedValue>(state.Output).Data == before);
}
