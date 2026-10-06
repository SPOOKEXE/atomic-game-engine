#include "../src/ImageGraphGroupHost.hpp"

#include "../src/ImageGraphComposerCadence.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("studio.imagegraph.group_host")
TEST_DEPENDS("engine.imagegraph.group_replay")
TEST_DEPENDS("engine.imagegraph.source_mode_transition")
TEST_DEPENDS("studio.imagegraph")

using namespace engine::imagegraph;

namespace {
	Document GroupDocument() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{2}},
			  {"subtype", EnumValue{0}},
			  {"vector_size", EnumValue{0}},
			  {"parent_value", .5}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		document.Groups = {
			{"group",
			 "Group",
			 {},
			 {{"input", "in", PortDirection::Input, "input"},
			  {"output", "out", PortDirection::Output, "output"}}}
		};
		document.Junctions = {
			{"in", "group", ValueType::Any, .5}, {"out", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {{"input", "value", "output", "value"}, {"output", "value", "out", "value"}};
		document.Outputs = {{"result", "output", "value"}};
		return document;
	}
}

TEST_CASE(
	"Studio Group host keeps declaration callbacks separate from seeks and restores history",
	"[studio][group_host]"
) {
	auto document = GroupDocument();
	document.Keyframes = {
		{"input", "input_type", 0, EnumValue{2}, "source", KeyframeEase{}},
		{"input", "input_type", 10, EnumValue{1}, "source", KeyframeEase{}}
	};
	document.Nodes[0].SourceAnimatedInputs = {"input_type"};
	document.Tracks = {{"input", "input_type", "wrap", -1}};
	document.Timeline = TimelineSettings{11};
	Plan plan;
	Diagnostic error;
	const auto compileStatus1 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus1 == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	request.Tick = 10;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "result", request, value, error) == Status::Ok);
	CHECK(value.Domain->Kind == SourceSocketKind::Boolean);
	const auto before = document;
	studio::ImageGraphHistory history;
	Value replacement = EnumValue{1};
	GroupRefreshEvent event;
	event.NodeId = "input";
	event.Reason = GroupRefreshReason::Edit;
	event.EditedPort = "input_type";
	event.LocalValue = &replacement;
	event.At = request;
	event.At.Tick = 5;
	event.LocalAnimated = true;
	INFO(error.Message);
	const bool editAccepted = host.Edit(document, history, 1, event, error);
	INFO(error.Message);
	REQUIRE(editAccepted);
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Float);
	const auto after = document;
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	host.Clear();
	request.Tick = 0;
	const auto compileStatus2 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus2 == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 3, request, error));
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	REQUIRE(history.Redo(document));
	CHECK(document == after);
}

TEST_CASE(
	"Studio Group edit failure leaves document history and cached declarations intact", "[studio][group_host]"
) {
	auto document = GroupDocument();
	Plan plan;
	Diagnostic error;
	const auto compileStatus3 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus3 == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	const auto before = document;
	const auto retained = host.Replay.RetainedBytes();
	studio::ImageGraphHistory history;
	Value invalid = std::string{"wrong"};
	GroupRefreshEvent event;
	event.NodeId = "input";
	event.EditedPort = "input_type";
	event.Reason = GroupRefreshReason::Edit;
	event.LocalValue = &invalid;
	event.At = request;
	CHECK_FALSE(host.Edit(document, history, 1, event, error));
	CHECK(error.Code != Status::Ok);
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	CHECK(host.Replay.RetainedBytes() == retained);
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
}

TEST_CASE(
	"Studio combined Range Any Trigger groups survive canvas history and preview routing",
	"[studio][group_host][imagegraph]"
) {
	Document document;
	document.FormatVersion = 9;
	const std::array<std::string, 3> names{"range", "any", "trigger"};
	const std::array<Value, 3> values{
		ArrayValue{ValueType::Scalar, {2.0, 8.0}},
		ArrayValue{
			ValueType::Any,
			{},
			{},
			{SourceArrayItem{ElementValue{std::string{"kept"}}},
			 SourceArrayItem{ElementValue{4.0}},
			 SourceArrayItem{ElementValue{true}}}
		},
		false
	};
	for (size_t index = 0; index < names.size(); ++index) {
		auto group = GroupDocument();
		const auto &name = names[index];
		for (auto &node : group.Nodes) {
			node.Id = name + "/" + node.Id;
			node.GroupId = name;
		}
		group.Nodes[0].Values[0].Data = EnumValue{index == 0 ? 1 : index == 1 ? 11 : 19};
		group.Nodes[0].Values[1].Data = EnumValue{index == 0 ? 1 : 0};
		group.Nodes[0].Values[3].Data = values[index];
		group.Groups[0].Id = name;
		for (auto &port : group.Groups[0].Ports) {
			port.JunctionId = name + "/" + port.JunctionId;
			port.ControlNodeId = name + "/" + port.ControlNodeId;
		}
		for (auto &junction : group.Junctions) {
			junction.Id = name + "/" + junction.Id;
			junction.GroupId = name;
			if (junction.Default) junction.Default = values[index];
		}
		for (auto &link : group.Links) {
			link.FromNode = name + "/" + link.FromNode;
			link.ToNode = name + "/" + link.ToNode;
		}
		document.Nodes.insert(document.Nodes.end(), group.Nodes.begin(), group.Nodes.end());
		document.Groups.insert(document.Groups.end(), group.Groups.begin(), group.Groups.end());
		document.Junctions.insert(document.Junctions.end(), group.Junctions.begin(), group.Junctions.end());
		document.Links.insert(document.Links.end(), group.Links.begin(), group.Links.end());
		document.Outputs.push_back({name, name + "/output", "value"});
	}
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string adapterError;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, adapterError));
	Document restored;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, restored, adapterError));
	CHECK(restored == document);
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(restored, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compiled == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(restored, plan, 1, request, error));
	CHECK(host.Replay.Find("range/input")->Domain.Display == SourceValueDisplay::Range);
	CHECK(host.Replay.Find("any/input")->Domain.Kind == SourceSocketKind::Any);
	CHECK(host.Replay.Find("trigger/input")->Domain.Kind == SourceSocketKind::Trigger);
	for (size_t index = 0; index < names.size(); ++index) {
		studio::ImageGraphPreviewValue preview;
		INFO(error.Message);
		REQUIRE(
			studio::EvaluateImageGraphPreview(restored, plan, names[index], request, preview, error) ==
			Status::Ok
		);
		CHECK(std::get<EvaluatedValue>(preview).Data == values[index]);
	}
	studio::ImageGraphHistory history;
	Value replacement = ArrayValue{ValueType::Scalar, {3.0, 9.0}};
	GroupRefreshEvent edit;
	edit.NodeId = "range/input";
	edit.EditedPort = "parent_value";
	edit.Reason = GroupRefreshReason::ParentEdit;
	edit.LocalValue = &replacement;
	edit.At = request;
	REQUIRE(host.Edit(restored, history, 1, edit, error));
	CHECK(restored != document);
	REQUIRE(history.Undo(restored));
	CHECK(restored == document);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(restored));
	CHECK(restored.Nodes[0].Values[3].Data == replacement);
}

TEST_CASE(
	"Studio Trigger press activates mode and persists one pulse with one undo",
	"[studio][group_host][trigger]"
) {
	auto document = GroupDocument();
	document.Nodes[0].Values[0].Data = EnumValue{19};
	document.Nodes[0].Values[3].Data = false;
	document.Nodes[0].SourceStaticInputs = {"parent_value"};
	const auto before = document;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphHistory history;
	Diagnostic error;
	Value pressed = true;
	GroupRefreshEvent event;
	event.NodeId = "input";
	event.EditedPort = "parent_value";
	event.Reason = GroupRefreshReason::ParentEdit;
	event.LocalValue = &pressed;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	const bool accepted = host.Edit(document, history, 1, event, error);
	INFO(error.Message);
	REQUIRE(accepted);
	CHECK(document.Nodes[0].SourceStaticInputs.empty());
	CHECK(document.Nodes[0].SourceAnimatedInputs == std::vector<std::string>{"parent_value"});
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(GetFrameTime(document.Keyframes[0]) == FrameTime{5, 0, false});
	Plan plan;
	const auto compileStatus4 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus4 == Status::Ok);
	for (const uint64_t tick : {0, 4, 5, 6, 5}) {
		EvaluationRequest request;
		request.Tick = tick;
		const bool prepareAccepted = host.Prepare(document, plan, 2, request, error);
		INFO(error.Message);
		REQUIRE(prepareAccepted);
		studio::ImageGraphPreviewValue preview;
		REQUIRE(
			studio::EvaluateImageGraphPreview(document, plan, "result", request, preview, error) == Status::Ok
		);
		CHECK(std::get<bool>(std::get<EvaluatedValue>(preview).Data) == (tick == 5));
	}
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(document.Keyframes.size() == 1);
}

TEST_CASE("Studio instance host keeps nearest getter mode and shared writer atomic", "[studio][group_host]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.invert", "", {}, {{"mix", .1}}},
		{"middle", "pc.invert", "", {}, {{"mix", .2}}},
		{"child", "pc.invert", "", {}, {{"mix", .9}}}
	};
	document.Nodes[0].SourceStaticInputs = {"mix"};
	document.Nodes[1].InstanceBase = "owner";
	document.Nodes[1].InstanceOverrides = {"mix"};
	document.Nodes[1].SourceAnimatedInputs = {"mix"};
	document.Nodes[2].InstanceBase = "middle";
	document.Outputs = {{"result", "child", "surface_out"}};
	document.Keyframes = {
		{"owner", "mix", 0, .1, "source", KeyframeEase{}},
		{"middle", "mix", 0, .25, "source", KeyframeEase{}},
		{"middle", "mix", 10, .75, "source", KeyframeEase{}}
	};
	document.Timeline = TimelineSettings{11};
	document.Tracks = {{"owner", "mix", "wrap", -1}, {"middle", "mix", "wrap", -1}};
	Plan plan;
	Diagnostic error;
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	const auto compileStatus5 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus5 == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	REQUIRE(host.Replay.Binding("child", "mix"));
	CHECK(host.Replay.Binding("child", "mix")->Getter == GroupSubtypeAnimator::Animated);
	CHECK(host.Replay.Binding("child", "mix")->Writer == GroupSubtypeAnimator::Static);
	CHECK(host.Replay.Binding("child", "mix")->OwnerId == "owner");
	const auto before = document;
	studio::ImageGraphHistory history;
	Value replacement = .6;
	GroupRefreshEvent event;
	event.NodeId = "child";
	event.EditedPort = "mix";
	event.LocalValue = &replacement;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	const bool accepted = host.Edit(document, history, 1, event, error);
	INFO(error.Message);
	REQUIRE(accepted);
	const auto checkOriginalWriter = [&] {
		CHECK(document.Nodes[0].Values[0].Data == Value{.1});
		CHECK(document.Nodes[0].SourceStaticInputs == std::vector<std::string>{"mix"});
		CHECK(document.Nodes[0].SourceAnimatedInputs.empty());
		const auto key =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &frame) {
				return frame.NodeId == "owner" && frame.Port == "mix" && GetFrameTime(frame) == FrameTime{};
			});
		REQUIRE(key != document.Keyframes.end());
		CHECK(key->Data == replacement);
	};
	checkOriginalWriter();
	CHECK(document.Nodes[2].InstanceBase == "middle");
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	checkOriginalWriter();
}

TEST_CASE(
	"Studio standalone Trigger button enables animation and writes one exact pulse",
	"[studio][group_host][trigger]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"button", "pc.trigger", "", {}, {{"trigger", false}}}};
	document.Nodes[0].SourceStaticInputs = {"trigger"};
	document.Outputs = {{"result", "button", "trigger"}};
	const auto before = document;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphHistory history;
	Diagnostic error;
	Value pressed = true;
	GroupRefreshEvent event;
	event.NodeId = "button";
	event.EditedPort = "trigger";
	event.LocalValue = &pressed;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	const bool accepted = host.Edit(document, history, 1, event, error);
	INFO(error.Message);
	REQUIRE(accepted);
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(document.Keyframes[0].Tick == 5);
	CHECK(document.Keyframes[0].Data == pressed);
	Plan plan;
	const auto compileStatus6 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus6 == Status::Ok);
	for (const uint64_t tick : {0, 4, 5, 6, 5}) {
		EvaluationRequest request;
		request.Tick = tick;
		const bool prepareAccepted = host.Prepare(document, plan, 2, request, error);
		INFO(error.Message);
		REQUIRE(prepareAccepted);
		EvaluatedValue pulse;
		REQUIRE(EvaluateValue(document, plan, "result", request, pulse, error) == Status::Ok);
		CHECK(std::get<bool>(pulse.Data) == (tick == 5));
	}
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(document.Keyframes[0].Tick == 5);
}

namespace {
	Document ColdGetterDocument() {
		Document document;
		document.FormatVersion = 9;
		Node gradient{
			"gradient",
			"pc.gradient_points_n",
			{},
			{},
			{{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}}
		};
		gradient.DynamicInputs = {
			{"point_i_0", ValueType::Vector2, Value{Vector2{99, 88}}},
			{"point_i_1", ValueType::Vector2, Value{Vector2{30, 3}}}
		};
		gradient.SourceStaticInputs = {"point_i_0"};
		gradient.SourceVec2Defaults.emplace().Inputs = {{"point_i_0", Vector2{7, 8}}};
		gradient.SourceSeparatedVec2Animators.emplace().Inputs = {{"point_i_0", {}, true, false}};
		document.Nodes.push_back(std::move(gradient));
		document.Outputs = {{"out", "gradient", "surface_out"}};
		return document;
	}
}

TEST_CASE(
	"Studio retains only observed constructor requests and projects their scalar storage for native save",
	"[studio][group_host][source_axis_read]"
) {
	auto document = ColdGetterDocument();
	const auto original = document;
	Plan plan;
	Diagnostic error;
	const auto compileStatus = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	CHECK_FALSE(host.Replay.SharedSubtype("gradient", "point_i_0"));
	EvaluationSnapshot inputs;
	REQUIRE(
		EvaluateNodeInputs(document, plan, "gradient", request, inputs, error) ==
		Status::SourceAxisInitializationRequired
	);
	CHECK(error.NodeId == "gradient");
	CHECK(error.Port == "point_i_0");
	const auto receipt = error;
	REQUIRE(host.RetainAxisRead(document, 1, request, error, error));
	CHECK(request.GroupReplay == &host.Replay);
	CHECK(document == original);
	REQUIRE(host.Replay.SharedSubtype("gradient", "point_i_0"));
	CHECK(host.Replay.SharedSubtype("gradient", "point_i_0")->SeparatedVec2->Initialized);
	CHECK_FALSE(host.RetainAxisRead(document, 1, request, receipt, error));
	REQUIRE(EvaluateNodeInputs(document, plan, "gradient", request, inputs, error) == Status::Ok);
	const auto point = std::find_if(inputs.Values().begin(), inputs.Values().end(), [](const auto &value) {
		return value.Port == "point_i_0";
	});
	REQUIRE(point != inputs.Values().end());
	CHECK(point->Data == Value{Vector2{7, 8}});
	Document saved;
	REQUIRE(host.ProjectForSave(document, 1, saved, error));
	CHECK(saved.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Initialized);
	Document reopened;
	REQUIRE(Read(Write(saved), reopened, error) == Status::Ok);
	CHECK(reopened == saved);
}

TEST_CASE(
	"Studio constructor requests reject stale state and live-budget refusal without replacing replay",
	"[studio][group_host][source_axis_read]"
) {
	auto document = ColdGetterDocument();
	Plan plan;
	Diagnostic error;
	const auto compileStatus = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	const Diagnostic receipt{
		Status::SourceAxisInitializationRequired, "gradient", "point_i_0", "constructor"
	};
	const auto held = host.Replay.RetainedBytes();
	const auto *requestReplay = request.GroupReplay;
	const auto requestRevision = request.GroupAuthoringRevision;
	CHECK_FALSE(host.RetainAxisRead(document, 1, request, receipt, error, 1));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(host.Replay.RetainedBytes() == held);
	CHECK_FALSE(host.Replay.SharedSubtype("gradient", "point_i_0"));
	CHECK(request.GroupReplay == requestReplay);
	CHECK(request.GroupAuthoringRevision == requestRevision);
	CHECK_FALSE(host.RetainAxisRead(document, 2, request, receipt, error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(host.Replay.RetainedBytes() == held);
	CHECK_FALSE(host.Replay.SharedSubtype("gradient", "point_i_0"));
	host.BorrowedBytes = Limits::MaximumEvaluationBytes - 1;
	CHECK_FALSE(host.RetainAxisRead(document, 1, request, receipt, error));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(host.Replay.RetainedBytes() == held);
	CHECK(request.GroupReplay == &host.Replay);
	host.BorrowedBytes = 0;
	Document saved = document;
	CHECK_FALSE(host.ProjectForSave(document, 1, saved, error, 1));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(saved == document);
	CHECK(host.Replay.RetainedBytes() == held);
	CHECK(request.GroupReplay == requestReplay);
	CHECK(request.GroupAuthoringRevision == requestRevision);
	Document staleSaved = document;
	CHECK_FALSE(host.ProjectForSave(document, 2, staleSaved, error));
	CHECK(staleSaved == document);
}

TEST_CASE(
	"Studio holds composer cadence while cold getter defaults are retained",
	"[studio][group_host][source_axis_read][composer_cadence]"
) {
	auto document = ColdGetterDocument();
	auto &gradient = document.Nodes.front();
	gradient.SourceStaticInputs.push_back("point_i_1");
	gradient.SourceVec2Defaults->Inputs.push_back({"point_i_1", Vector2{30, 3}});
	gradient.SourceSeparatedVec2Animators->Inputs.push_back({"point_i_1", {}, true, false});
	Plan plan;
	Diagnostic error;
	const auto compileStatus = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus == Status::Ok);
	studio::ImageGraphGroupHost host;
	studio::detail::ImageGraphComposerCadence cadence;
	const studio::detail::ImageGraphComposerCadence::Identity identity{
		engine::core::Name("cold-getter-preview"), "out", 1, 0, 0
	};
	studio::detail::ImageGraphObservations observations;
	std::tm calendar{};
	EvaluationSnapshot inputs;
	EvaluationRequest request;

	for (uint64_t pulse = 0; pulse < 3; ++pulse) {
		const FrameTime playback{5 + pulse};
		observations.Capture(1, playback, "scene", static_cast<double>(playback.Tick), calendar);
		const auto frame = cadence.Begin(identity, playback, observations);
		CHECK(frame == FrameTime{5});
		REQUIRE(SetFrameTime(request, frame));
		REQUIRE(host.Prepare(document, plan, 1, request, error));
		const auto status = EvaluateNodeInputs(document, plan, "gradient", request, inputs, error);
		if (pulse < 2) {
			REQUIRE(status == Status::SourceAxisInitializationRequired);
			CHECK(error.NodeId == "gradient");
			CHECK(error.Port == (pulse == 0 ? "point_i_0" : "point_i_1"));
			const auto receipt = error;
			REQUIRE(host.RetainAxisRead(document, 1, request, receipt, error));
			cadence.Pending();
			CHECK(cadence.Frame == FrameTime{5});
			CHECK_FALSE(cadence.Displayed);
		} else {
			INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
			REQUIRE(status == Status::Ok);
			CHECK(cadence.Complete(frame));
			CHECK(cadence.Displayed == FrameTime{5});
			CHECK_FALSE(cadence.Held);
		}
	}

	observations.Capture(1, FrameTime{8}, "scene", 8, calendar);
	CHECK(cadence.Begin(identity, FrameTime{8}, observations) == FrameTime{8});
}

TEST_CASE(
	"Studio native history restores captured scalar sharing before sampling",
	"[studio][group_host][source_animator_persistence]"
) {
	auto document = ColdGetterDocument();
	document.Nodes.front().Id = "base";
	Node copy = document.Nodes.front();
	copy.Id = "copy";
	copy.InstanceBase = "base";
	copy.InstanceOverrides = {"point_i_0"};
	document.Nodes.push_back(std::move(copy));
	document.Outputs.front().NodeId = "base";
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, request, error));
	CHECK(host.Replay.Binding("copy", "point_i_0")->Axes.Storage == GroupAxisStorage::Uninitialized);
	document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Initialized = true;
	document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys = {
		{"base", "point_i_0", 0, 42.0, "source", KeyframeEase{}}
	};
	document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys = {
		{"base", "point_i_0", 0, 43.0, "source", KeyframeEase{}}
	};
	Document before;
	REQUIRE(ProjectGroupReplay(document, host.Replay, 1, before, error) == Status::Ok);
	const std::array targets{GroupInstanceRecapture{"copy", "point_i_0"}};
	GroupReplayState recaptured;
	REQUIRE(RecaptureGroupInstances(document, targets, host.Replay, 1, recaptured, error) == Status::Ok);
	Document after;
	REQUIRE(ProjectGroupReplay(document, recaptured, 1, after, error) == Status::Ok);
	studio::ImageGraphHistory history;
	REQUIRE(history.TryRecord(before, after));
	document = after;
	host.Clear();
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 2, request, error));
	CHECK(host.Replay.Binding("copy", "point_i_0")->Axes.Storage == GroupAxisStorage::Shared);
	REQUIRE(history.Undo(document));
	host.Clear();
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 3, request, error));
	CHECK(host.Replay.Binding("copy", "point_i_0")->Axes.Storage == GroupAxisStorage::Uninitialized);
	REQUIRE(history.Redo(document));
	host.Clear();
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 4, request, error));
	CHECK(host.Replay.Binding("copy", "point_i_0")->Axes.Storage == GroupAxisStorage::Shared);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "copy", request, snapshot, error) == Status::Ok);
	const auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &item) {
		return item.Port == "point_i_0";
	});
	REQUIRE(value != snapshot.Values().end());
	CHECK(value->Data == Value{Vector2{42, 43}});
}
