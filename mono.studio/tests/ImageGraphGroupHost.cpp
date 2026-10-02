#include "../src/ImageGraphGroupHost.hpp"

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
