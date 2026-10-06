#include "../src/SourceSeparatedVec2.hpp"

#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_separated_vec2")
using namespace engine::imagegraph;
namespace {
	Keyframe Scalar(uint64_t tick, double value) {
		Keyframe key{"mirror", "center", tick, value, "source", KeyframeEase{}};
		return key;
	}
	Node Separated(bool separated = true) {
		Node node{"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{.9, .9}}}};
		node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
		node.SourceSeparatedVec2Animators->Inputs.back().Separated = separated;
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
	CHECK(text.find("source_vec2_axis \"mirror\" \"center\" x\n") != std::string::npos);
	CHECK(text.find("source_vec2_axis \"mirror\" \"center\" x 0") == std::string::npos);
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

TEST_CASE("Inactive separated Vec2 axes persist with matched explicit mode markers", "[mirror_axes]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {Separated(false)};
	const auto text = Write(document);
	REQUIRE(!text.empty());
	CHECK(text.find("source_vec2_axis \"mirror\" \"center\" x 0\n") != std::string::npos);
	CHECK(text.find("source_vec2_axis \"mirror\" \"center\" y 0\n") != std::string::npos);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);

	const auto prior = restored;
	auto mismatched = text;
	const auto yAxis = mismatched.find("source_vec2_axis \"mirror\" \"center\" y 0");
	REQUIRE(yAxis != std::string::npos);
	mismatched[yAxis + std::string_view("source_vec2_axis \"mirror\" \"center\" y ").size()] = '1';
	CHECK(Read(mismatched, restored, diagnostic) == Status::Malformed);
	CHECK(restored == prior);
}

TEST_CASE("Inactive Vec2 mode transitions change combined keys and retain dormant axes", "[mirror_axes]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		Separated(false), {"source", "image.captured", "", {}, {{"source_id", std::string("source")}}}
	};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	document.Nodes[0].SourceStaticInputs = {"center"};
	document.Keyframes = {{"mirror", "center", 0, Vector2{.9, .8}, "source", KeyframeEase{}}};
	document.Tracks = {{"mirror", "center", "hold"}};
	document.Timeline = TimelineSettings{};
	document.Timeline->Frames = 20;
	const auto axes = document.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes;
	GroupReplayState empty, replay;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(document, empty, 1, replay, diagnostic) == Status::Ok);
	Document changed;
	const SourceModeTransition enable{"mirror", "center", true, FrameTime{5, 0, false}};
	REQUIRE(ToggleSourceInputMode(document, replay, 1, enable, changed, diagnostic) == Status::Ok);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(changed.Keyframes[0].Tick == 5);
	CHECK(changed.Keyframes[0].Data == document.Keyframes[0].Data);
	CHECK(changed.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Axes == axes);
	CHECK_FALSE(changed.Nodes[0].SourceSeparatedVec2Animators->Inputs[0].Separated);
}

TEST_CASE("Inactive Vec2 axes select capture codec v4 and retain mode on read", "[mirror_axes]") {
	SourceBuiltinRandomCapture capture;
	capture.Authored = Separated(false);
	capture.Inputs = {{"center", Vector2{4, 2}}};
	std::string text;
	Diagnostic diagnostic;
	const auto inactive = capture;
	capture.Authored.SourceSeparatedVec2Animators->Inputs[0].Separated = true;
	capture.ProcessorRow = 1;
	const std::array captures{inactive, capture};
	REQUIRE(WriteBuiltinRandomCapture(captures, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 4\n"));
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.size() == 2);
	CHECK(restored[0] == inactive);
	CHECK(restored[1] == capture);
}

namespace {
	Keyframe AxisKey(std::string port, uint64_t tick, double value) {
		Keyframe key{"get", std::move(port), tick, value, "source", KeyframeEase{}};
		return key;
	}
	Document MatrixGetAxes(bool linked = false, bool dormant = false) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"get",
			 "pc.matrix_get",
			 "",
			 {},
			 {{"matrix", MatrixValue{2, 1, {13, 29}}}, {"position", Vector2{0, 0}}}}
		};
		auto &get = document.Nodes.front();
		get.SourceInputExpressions = {{"position", "value", true}};
		get.SourceAnimatedInputs = {"position"};
		SourceSeparatedVec2Animator axes;
		axes.Port = "position";
		axes.Separated = !dormant;
		axes.Axes[0].Keys = {AxisKey("position", 0, 0), AxisKey("position", 10, linked ? 0 : 1)};
		axes.Axes[1].Keys = {AxisKey("position", 0, 0), AxisKey("position", 10, 0)};
		if (dormant) axes.Axes[0].Keys.front().SourceDriver = KeyframeAudioDriver{"missing", "rms"};
		get.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		document.Tracks = {{"get", "position", "hold"}};
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 20;
		document.Outputs = {{"cell", "get", "output"}};
		if (linked) {
			document.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 0.0}}});
			document.Links = {{"position", "vector", "get", "position"}};
		}
		return document;
	}
	Value EvaluateCell(const Document &document, uint64_t tick) {
		Diagnostic diagnostic;
		Plan plan;
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue output;
		const auto status = EvaluateValue(document, plan, "cell", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output.Data;
	}
}

TEST_CASE("Catalogue IVec2 axes drive a matrix getter", "[mirror_axes]") {
	const auto *entry = FindCatalogueEntry("pc.matrix_get");
	REQUIRE(entry);
	const auto *position = FindCatalogueInput(*entry, "position");
	REQUIRE(position);
	CHECK(position->SourceKind == "IVec2");
	const auto document = MatrixGetAxes();
	CHECK(EvaluateCell(document, 0) == Value{13.0});
	CHECK(EvaluateCell(document, 10) == Value{29.0});
}

TEST_CASE("Linked Vec2 producer takes precedence over retained separated axes", "[mirror_axes]") {
	const auto document = MatrixGetAxes(true);
	CHECK(EvaluateCell(document, 10) == Value{29.0});
}

TEST_CASE("Inactive axes skip an unsupported driver and match ordinary combined input", "[mirror_axes]") {
	auto document = MatrixGetAxes(false, true);
	document.Keyframes = {
		{"get", "position", 0, Vector2{0, 0}, "source", KeyframeEase{}},
		{"get", "position", 10, Vector2{0, 0}, "source", KeyframeEase{}}
	};
	const Value output = EvaluateCell(document, 5);
	document.Nodes.front().SourceSeparatedVec2Animators = {};
	CHECK(EvaluateCell(document, 5) == output);
	CHECK(output == Value{13.0});
}

TEST_CASE("Declared dynamic Vec2 template accepts many axes and round trips owned storage", "[mirror_axes]") {
	Node node{"points", "pc.gradient_points_n", "", {}, {}, {}};
	node.SourceSeparatedVec2Animators.emplace();
	for (size_t index = 0; index < 6; ++index) {
		const std::string suffix = std::to_string(index);
		const std::string port = "point_i_" + suffix;
		node.DynamicInputs.push_back({port, ValueType::Vector2, Vector2{}});
		SourceSeparatedVec2Animator axes;
		axes.Port = port;
		axes.Axes[0].Keys = {Keyframe{"points", port, 0, double(index), "source", KeyframeEase{}}};
		axes.Axes[1].Keys = {Keyframe{"points", port, 0, 0.0, "source", KeyframeEase{}}};
		node.SourceSeparatedVec2Animators->Inputs.push_back(std::move(axes));
	}
	Diagnostic diagnostic;
	size_t keyCount = 0;
	CHECK(detail::SourceSeparatedVec2InputCount(node) == 7);
	CHECK(detail::ValidateSeparatedVec2(node, keyCount, diagnostic) == Status::Ok);
	CHECK(keyCount == 12);

	Document document;
	document.FormatVersion = 9;
	document.Nodes = {node};
	const auto text = Write(document);
	REQUIRE(!text.empty());
	Document restored;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);

	SourceBuiltinRandomCapture capture;
	capture.Authored = node;
	capture.Inputs = {{"point_i_0", Vector2{0, 0}}};
	std::string captureText;
	REQUIRE(WriteBuiltinRandomCapture(std::span(&capture, 1), captureText, diagnostic) == Status::Ok);
	std::vector<SourceBuiltinRandomCapture> captures;
	REQUIRE(ReadBuiltinRandomCapture(captureText, captures, diagnostic) == Status::Ok);
	REQUIRE(captures.size() == 1);
	CHECK(captures.front() == capture);

	auto undeclared = node;
	undeclared.DynamicInputs.pop_back();
	undeclared.SourceSeparatedVec2Animators->Inputs.back().Port = "point_i_6";
	keyCount = 0;
	CHECK(detail::ValidateSeparatedVec2(undeclared, keyCount, diagnostic) == Status::UnknownPort);
	CHECK(keyCount == 0);
}

TEST_CASE("Solid dimension axes change output size at their authored ticks", "[mirror_axes]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{20, 40, 60, 255}},
		  {"empty", false}}}
	};
	Node &solid = document.Nodes.front();
	solid.SourceAnimatedInputs = {"dimension"};
	SourceSeparatedVec2Animator axes;
	axes.Port = "dimension";
	axes.Axes[0].Keys = {
		Keyframe{"solid", "dimension", 0, 2.0, "source", KeyframeEase{}},
		Keyframe{"solid", "dimension", 10, 4.0, "source", KeyframeEase{}}
	};
	axes.Axes[1].Keys = {Keyframe{"solid", "dimension", 0, 1.0, "source", KeyframeEase{}}};
	solid.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
	document.Tracks = {{"solid", "dimension", "hold"}};
	document.Timeline = TimelineSettings{};
	document.Timeline->Frames = 20;
	document.Outputs = {{"image", "solid", "surface_out"}};

	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image first, last;
	EvaluationRequest request;
	request.Tick = 0;
	REQUIRE(Evaluate(document, plan, "image", request, first, diagnostic) == Status::Ok);
	request.Tick = 10;
	REQUIRE(Evaluate(document, plan, "image", request, last, diagnostic) == Status::Ok);
	CHECK(first.Width == 2);
	CHECK(first.Height == 1);
	CHECK(last.Width == 4);
	CHECK(last.Height == 1);

	solid.SourceInputExpressions = {{"dimension", "value[0] + value[1]", true}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image expressed;
	const auto status = Evaluate(document, plan, "image", request, expressed, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(expressed.Width == 5);
	CHECK(expressed.Height == 5);
}

TEST_CASE("Active axes leave unsupported combined drivers dormant", "[mirror_axes]") {
	auto document = MatrixGetAxes();
	Keyframe combined{"get", "position", 0, Vector2{0, 0}, "source", KeyframeEase{}};
	combined.SourceDriver = KeyframeAudioDriver{"missing", "rms"};
	document.Keyframes = {combined};
	CHECK(EvaluateCell(document, 10) == Value{29.0});
}
