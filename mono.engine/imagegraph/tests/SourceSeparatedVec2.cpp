#include "../src/SourceSeparatedVec2.hpp"

#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_separated_vec2")
using namespace engine::imagegraph;
namespace {
	Keyframe Scalar(uint64_t tick, double value) {
		Keyframe key{"mirror", "center", tick, value, "source", KeyframeEase{}};
		return key;
	}
	Node Separated() {
		Node node{"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{.9, .9}}}};
		node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
		auto &axes = node.SourceSeparatedVec2Animators->Inputs.front().Axes;
		axes[0].Keys = {Scalar(0, .25), Scalar(10, .75)};
		axes[1].Keys = {Scalar(0, 3)};
		return node;
	}
}
TEST_CASE("Separated getter and original writer modes retain distinct source fast paths", "[mirror_axes]") {
	SourceScalarAnimator axis{{Scalar(10, 2), Scalar(0, 1)}};
	EvaluationRequest request;
	request.Tick = 12;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Diagnostic diagnostic;
	double value = 9;
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, false, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == 2);
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, true, false, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == 2);
	axis.Keys.resize(1);
	axis.Keys.front().SourceDriver = KeyframeLinearDriver{.1};
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, true, false, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == 3.2);
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, false, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == 2);
}
TEST_CASE(
	"Empty separated storage distinguishes active scalar zero and undefined static first key", "[mirror_axes]"
) {
	SourceScalarAnimator axis;
	EvaluationRequest request;
	detail::EvaluationBudget budget(0);
	Diagnostic diagnostic;
	double value = 9;
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, true, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == 0);
	value = 9;
	CHECK(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, false, true, budget, value, diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(value == 9);
}
TEST_CASE(
	"Separated scalar source interpolation end modes and cut precedence use shared math", "[mirror_axes]"
) {
	const auto node = Separated();
	const auto &axis = node.SourceSeparatedVec2Animators->Inputs.front().Axes[0];
	EvaluationRequest request;
	request.Tick = 5;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Diagnostic diagnostic;
	double value = 9;
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, true, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == .5);
	request.Tick = 12;
	AnimationTrack track{"mirror", "center", "loop"};
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, &track, nullptr, request, true, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == .35);
	track.End = "ping";
	REQUIRE(
		detail::SampleSeparatedScalar(
			axis, &track, nullptr, request, true, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == .65);
	auto cut = axis;
	cut.Keys[0].Ease->OutType = "cut";
	cut.Keys[1].Ease->InType = "cut";
	request.Tick = 5;
	REQUIRE(
		detail::SampleSeparatedScalar(
			cut, nullptr, nullptr, request, true, true, budget, value, diagnostic
		) == Status::Ok
	);
	CHECK(value == .25);
}
TEST_CASE(
	"Separated scalar workspace refusal preserves prior sample and releases reservations", "[mirror_axes]"
) {
	const auto node = Separated();
	const auto &axis = node.SourceSeparatedVec2Animators->Inputs.front().Axes[0];
	EvaluationRequest request;
	request.Tick = 5;
	detail::EvaluationBudget budget(1);
	Diagnostic diagnostic;
	double value = 9;
	CHECK(
		detail::SampleSeparatedScalar(
			axis, nullptr, nullptr, request, true, true, budget, value, diagnostic
		) == Status::LimitExceeded
	);
	CHECK(value == 9);
	CHECK(budget.Used() == 0);
}
TEST_CASE("Separated axis payload is independently owned and counts retained capacity", "[mirror_axes]") {
	auto original = Separated();
	auto copy = original;
	REQUIRE(copy == original);
	const auto before = NodeClonePayloadBytes(original);
	REQUIRE(before);
	original.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data = .1;
	CHECK(copy != original);
	CHECK(std::get<double>(copy.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data) == .25);
	original.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys.reserve(64);
	const auto retained = detail::SeparatedVec2Bytes(original, true);
	const auto clone = detail::SeparatedVec2Bytes(original, false);
	REQUIRE(retained);
	REQUIRE(clone);
	CHECK(*retained >= *clone + 62 * sizeof(Keyframe));
}
TEST_CASE(
	"Separated axis native records preserve full scalar key controls and dormant Vec2", "[mirror_axes]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Separated()};
	auto &key = document.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0];
	key.SourceKeyId = "archive:input:0:animator:x:key:0";
	key.Kind = KeyframeKind::Adder;
	key.Subframe = .5;
	key.NegativeFrame = true;
	key.SourceDriver = KeyframeSnapDriver{.1};
	document.Keyframes = {{"mirror", "center", 0, Vector2{.7, .8}, "step"}};
	const auto text = Write(document);
	REQUIRE(!text.empty());
	CHECK(text.find("source_vec2_axis \"mirror\" \"center\" x") != std::string::npos);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	const auto old = restored;
	CHECK(
		Read(
			text + "source_vec2_axis \"mirror\" \"center\" x\nsource_vec2_axis_end\n", restored, diagnostic
		) == Status::Malformed
	);
	CHECK(restored == old);
	CHECK(
		Read(text.substr(0, text.rfind("source_vec2_axis_end")), restored, diagnostic) == Status::Malformed
	);
	CHECK(restored == old);
}
TEST_CASE("Separated scalar malformed and aggregate admission is atomic", "[mirror_axes]") {
	auto node = Separated();
	size_t count = Limits::MaximumKeyframes - 1;
	Diagnostic diagnostic;
	CHECK(detail::ValidateSeparatedVec2(node, count, diagnostic) == Status::LimitExceeded);
	CHECK(count == Limits::MaximumKeyframes - 1);
	count = 0;
	node.SourceSeparatedVec2Animators->Inputs[0].Axes[1].Keys[0].Data = Vector2{1, 2};
	CHECK(detail::ValidateSeparatedVec2(node, count, diagnostic) == Status::InvalidValue);
	CHECK(count == 0);
	CHECK_FALSE(NodeClonePayloadBytes(node));
}

TEST_CASE(
	"Complete authored capture v3 owns split axes and preserves malformed replacements", "[mirror_axes]"
) {
	SourceBuiltinRandomCapture capture;
	capture.Authored = Separated();
	capture.Inputs = {{"center", Vector2{4, 2}}};
	capture.Authored.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].SourceDriver =
		KeyframeSnapDriver{.1};
	const std::vector captures{capture};
	std::string text;
	Diagnostic diagnostic;
	REQUIRE(WriteBuiltinRandomCapture(captures, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 3\n"));
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	REQUIRE(restored == captures);
	capture.Authored.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data = .9;
	CHECK(restored == captures);
	const auto before = restored;
	auto malformed = text;
	const auto scalar = malformed.find(" d ");
	REQUIRE(scalar != std::string::npos);
	malformed[scalar + 1] = 'a';
	CHECK(ReadBuiltinRandomCapture(malformed, restored, diagnostic) == Status::Malformed);
	CHECK(restored == before);
	CHECK(ReadBuiltinRandomCapture(text, restored, diagnostic, 1) == Status::LimitExceeded);
	CHECK(restored == before);
}

TEST_CASE(
	"Source duplicate and unsorted scalar storage preserves final key-map overwrites", "[mirror_axes]"
) {
	SourceScalarAnimator axis{{Scalar(5, .25), Scalar(10, .75), Scalar(5, .5)}};
	EvaluationRequest request;
	TimelineSettings timeline;
	timeline.Frames = 20;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Diagnostic diagnostic;
	double value = 9;
	const auto sample = [&](uint64_t tick) {
		request.Tick = tick;
		REQUIRE(
			detail::SampleSeparatedScalar(
				axis, nullptr, &timeline, request, true, true, budget, value, diagnostic
			) == Status::Ok
		);
	};
	sample(0);
	CHECK(value == .25);
	sample(4);
	CHECK(value == .25);
	// updateKeyMap's final tail starts at the last stored key (5), overwriting
	// the earlier [5,10) interval. Sorting or deduplicating changes this value.
	sample(5);
	CHECK(value == .5);
	sample(7);
	CHECK(value == .5);
	sample(10);
	CHECK(value == .5);
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Separated()};
	document.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes[0] = axis;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	size_t count = 0;
	CHECK(detail::ValidateSeparatedVec2(restored.Nodes[0], count, diagnostic) == Status::Ok);
	CHECK(count == 4);
	request.Subframe = .5;
	const double before = value;
	CHECK(
		detail::SampleSeparatedScalar(
			axis, nullptr, &timeline, request, true, true, budget, value, diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(value == before);
}

TEST_CASE("Split source mode transitions append and retime without normalizing storage", "[mirror_axes]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		Separated(), {"source", "image.captured", "", {}, {{"source_id", std::string("source")}}}
	};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	document.Nodes[0].SourceStaticInputs = {"center"};
	document.Keyframes = {{"mirror", "center", 0, Vector2{.9, .9}, "source", KeyframeEase{}}};
	document.Tracks = {{"mirror", "center", "hold"}};
	document.Timeline = TimelineSettings{};
	document.Timeline->Frames = 20;
	GroupReplayState empty, replay;
	Diagnostic diagnostic;
	Plan beforePlan;
	REQUIRE(Compile(document, beforePlan, diagnostic) == Status::Ok);
	REQUIRE(RebindGroupReplay(document, empty, 1, replay, diagnostic) == Status::Ok);
	Document enabled;
	GroupReplayState enabledReplay;
	const SourceModeTransition enable{"mirror", "center", true, FrameTime{5, 0, false}};
	REQUIRE(
		ToggleSourceInputMode(document, replay, 1, enable, enabled, enabledReplay, diagnostic) == Status::Ok
	);
	const auto &axes = enabled.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes;
	REQUIRE(axes[0].Keys.size() == 3);
	CHECK(axes[0].Keys[0].Tick == 5);
	CHECK(axes[0].Keys[1].Tick == 10);
	CHECK(axes[0].Keys[2].Tick == 5);
	CHECK(std::get<double>(axes[0].Keys[2].Data) == .5);
	REQUIRE(axes[1].Keys.size() == 2);
	CHECK(axes[1].Keys[0].Tick == 5);
	CHECK(axes[1].Keys[1].Tick == 5);
	CHECK(enabled.Keyframes == document.Keyframes);
	CHECK(enabled.Nodes[0].Values == document.Nodes[0].Values);
	Document saved;
	REQUIRE(Read(Write(enabled), saved, diagnostic) == Status::Ok);
	CHECK(saved == enabled);
	Plan enabledPlan;
	REQUIRE(Compile(saved, enabledPlan, diagnostic) == Status::Ok);
	Document disabled;
	const SourceModeTransition disable{"mirror", "center", false, FrameTime{7, 0, false}};
	REQUIRE(ToggleSourceInputMode(enabled, enabledReplay, 1, disable, disabled, diagnostic) == Status::Ok);
	const auto &collapsed = disabled.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes;
	REQUIRE(collapsed[0].Keys.size() == 1);
	CHECK(collapsed[0].Keys[0].Tick == 0);
	// The writer flag changes first, so a multi-key static writer reads stored key zero.
	CHECK(std::get<double>(collapsed[0].Keys[0].Data) == .25);
	CHECK_FALSE(collapsed[0].Keys[0].SourceDriver);
	CHECK(disabled.Keyframes == document.Keyframes);
	const auto before = disabled;
	CHECK(
		ToggleSourceInputMode(document, replay, 1, enable, disabled, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(disabled == before);
}
