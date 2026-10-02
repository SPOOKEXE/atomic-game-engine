#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/ImageGraphGroupHost.hpp"
#include "../src/ImageGraphObservations.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.source_inputs")
TEST_DEPENDS("studio.imagegraph")

using namespace engine::imagegraph;

TEST_CASE(
	"Source grouped inputs preserve complete templates and undo deleted authored socket state",
	"[studio][source_inputs]"
) {
	for (const std::string type : {"pc.struct", "pc.lua_compute", "pc.pb_draw_rectangle"}) {
		INFO(type);
		Document document;
		document.Nodes = {{"node", type, {}, {}, {}}};
		Diagnostic error;
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 2, error));
		CHECK(document.Nodes[0].DynamicInputs.size() == entry->DynamicTemplate.size() * 2);
		for (const auto &input : document.Nodes[0].DynamicInputs) {
			size_t group = 99;
			REQUIRE(FindDynamicTemplate(*entry, input.Id, group));
			CHECK(group < 2);
		}
		const auto removed = document.Nodes[0].DynamicInputs.back().Id;
		document.Nodes[0].InstanceOverrides.push_back(removed);
		document.Links.push_back({"upstream", "value", "node", removed});
		document.Nodes[0].SourceInputExpressions.push_back({removed, {}});
		const auto before = document;
		studio::ImageGraphHistory history;
		REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
			return studio::SetSourceImageGraphDynamicGroupCount(staged, "node", 1, error);
		}));
		CHECK(document.Links.empty());
		CHECK(document.Nodes[0].InstanceOverrides.empty());
		CHECK(document.Nodes[0].SourceInputExpressions.empty());
		REQUIRE(history.Undo(document));
		CHECK(document == before);
		REQUIRE(history.Redo(document));
		CHECK(document.Nodes[0].DynamicInputs.size() == entry->DynamicTemplate.size());
		const auto prior = document;
		CHECK_FALSE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 10000, error));
		CHECK(document == prior);
		if (type.starts_with("pc.pb_draw_")) {
			std::erase_if(document.Nodes[0].DynamicInputs, [&](const auto &input) {
				size_t group = 0;
				return FindDynamicTemplate(*entry, input.Id, group)->SourceIndex < 0;
			});
			REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 1, error));
			CHECK(document.Nodes[0].DynamicInputs.size() == entry->DynamicTemplate.size());
			REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 64, error));
			CHECK(document.Nodes[0].DynamicInputs.size() == entry->DynamicTemplate.size() * 64);
			CHECK_FALSE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 65, error));
		}
	}
}

TEST_CASE(
	"Struct source groups author literal values while retaining Any sockets across save and canvas",
	"[studio][source_inputs]"
) {
	Document document;
	document.Nodes = {{"node", "pc.struct", {}, {}, {}}};
	Diagnostic error;
	REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 1, error));
	auto key = document.Nodes[0].DynamicInputs[0];
	key.Default = std::string("answer");
	REQUIRE(studio::SetImageGraphDynamicInput(document, "node", key, error));
	auto value = document.Nodes[0].DynamicInputs[1];
	value.Default = 42.5;
	REQUIRE(studio::SetImageGraphDynamicInput(document, "node", value, error));
	CHECK(document.Nodes[0].DynamicInputs[1].Type == ValueType::Any);
	document.Outputs = {{"result", "node", "struct"}};
	Plan plan;
	const auto compileStatus1 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus1 == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "result", {}, result, error) == Status::Ok);
	const auto &structure = std::get<StructValue>(result.Data);
	REQUIRE(structure.Data);
	REQUIRE(structure.Data->Fields.size() == 1);
	CHECK(structure.Data->Fields[0].first == "answer");
	CHECK(std::get<double>(structure.Data->Fields[0].second) == 42.5);
	const std::string encoded = Write(document);
	Document restored;
	REQUIRE(Read(encoded, restored, error) == Status::Ok);
	CHECK(restored == document);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, failure));
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, restored, failure));
	CHECK(restored == document);
}

TEST_CASE(
	"Source dynamic argument keys enable the original animator and remain one undo transaction",
	"[studio][source_inputs][group_host]"
) {
	Document document;
	document.Nodes = {{"node", "pc.lua_compute", {}, {}, {}}};
	document.Outputs = {{"result", "node", "return_value"}};
	document.Timeline = TimelineSettings{10};
	Diagnostic error;
	REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 1, error));
	const auto before = document;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphHistory history;
	Value value = 9.5;
	GroupRefreshEvent event;
	event.NodeId = "node";
	event.EditedPort = "argument_value_0";
	event.LocalValue = &value;
	event.LocalAnimated = true;
	event.At.Tick = 3;
	const bool editAccepted = host.Edit(document, history, 1, event, error);
	INFO(error.Message);
	REQUIRE(editAccepted);
	REQUIRE_FALSE(document.Keyframes.empty());
	const auto key =
		std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &frame) {
			return frame.NodeId == "node" && frame.Port == "argument_value_0" && frame.Tick == 3;
		});
	REQUIRE(key != document.Keyframes.end());
	CHECK(std::get<double>(key->Data) == 9.5);
	CHECK(
		studio::ImageGraphGroupHost::Mode(document, document.Nodes[0], "argument_value_0") ==
		GroupSubtypeAnimator::Animated
	);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	REQUIRE(studio::SetImageGraphKeyframe(document, "node", "argument_value_0", 4, "source", error));
	CHECK(std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
		return key.Port == "argument_value_0" && key.Tick == 4;
	}));
}

TEST_CASE(
	"Lua argument type changes update durable sockets while preserving raw animator values",
	"[studio][source_inputs]"
) {
	Document document;
	document.Nodes = {{"node", "pc.lua_compute", {}, {}, {}}};
	document.Outputs = {{"result", "node", "return_value"}};
	document.Timeline = TimelineSettings{10};
	Diagnostic error;
	REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "node", 1, error));
	const auto find = [&](std::string_view id) -> DynamicInput {
		return *std::find_if(
			document.Nodes[0].DynamicInputs.begin(),
			document.Nodes[0].DynamicInputs.end(),
			[&](const auto &input) { return input.Id == id; }
		);
	};
	REQUIRE(studio::SetImageGraphKeyframe(document, "node", "argument_value_0", 1, "source", error));
	document.Links = {{"prior", "value", "node", "argument_value_0"}};
	const auto before = document;
	studio::ImageGraphHistory history;
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		auto selector = find("argument_type_0");
		selector.Default = EnumValue{1};
		return studio::SetImageGraphDynamicInput(staged, "node", selector, error);
	}));
	CHECK(find("argument_value_0").Type == ValueType::Text);
	CHECK(document.Links.empty());
	const auto argumentKey =
		std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
			return key.Port == "argument_value_0" && key.Tick == 1;
		});
	REQUIRE(argumentKey != document.Keyframes.end());
	CHECK(std::get<double>(argumentKey->Data) == 0.0);
	auto argument = find("argument_value_0");
	argument.Default = std::string("hello");
	REQUIRE(studio::SetImageGraphDynamicInput(document, "node", argument, error));
	studio::ImageGraphGroupHost host;
	Value text = std::string("world");
	GroupRefreshEvent textEdit;
	textEdit.NodeId = "node";
	textEdit.EditedPort = "argument_value_0";
	textEdit.LocalValue = &text;
	const bool sourceTextAccepted = host.Edit(document, history, 2, textEdit, error);
	INFO(error.Message);
	REQUIRE(sourceTextAccepted);
	const auto textKey =
		std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
			return key.Port == "argument_value_0" && key.Tick == 0;
		});
	REQUIRE(textKey != document.Keyframes.end());
	CHECK(std::get<std::string>(textKey->Data) == "world");
	Plan plan;
	const auto compileStatus2 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus2 == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	CHECK(restored == document);
	for (const int selected : {2, 3}) {
		auto selector = find("argument_type_0");
		selector.Default = EnumValue{selected};
		REQUIRE(studio::SetImageGraphDynamicInput(document, "node", selector, error));
		CHECK(find("argument_value_0").Type == (selected == 2 ? ValueType::Image : ValueType::Struct));
		REQUIRE(find("argument_value_0").Default);
		CHECK(std::get<std::string>(*find("argument_value_0").Default) == "hello");
		const auto compileStatus3 = Compile(document, plan, error);
		INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(compileStatus3 == Status::Ok);
	}
	REQUIRE(history.Undo(document));
	REQUIRE(history.Undo(document));
	CHECK(document == before);
}

TEST_CASE(
	"GM room socket bindings preserve exact layer identities across native and canvas saves",
	"[studio][source_inputs]"
) {
	Document document;
	document.Nodes = {{"room", "pc.gmroom", {}, {}, {}}};
	Diagnostic error;
	REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "room", 1, error));
	auto input = document.Nodes[0].DynamicInputs[0];
	input.SourceLayerName = "Background / exact layer";
	REQUIRE(studio::SetImageGraphDynamicInput(document, "room", input, error));
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	CHECK(restored == document);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, failure));
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, restored, failure));
	CHECK(restored == document);
	input.SourceLayerName.assign(Limits::MaximumTextBytes + 1, 'x');
	const auto before = document;
	CHECK_FALSE(studio::SetImageGraphDynamicInput(document, "room", input, error));
	CHECK(document == before);
}

TEST_CASE(
	"PCX observations retain exact calendar and elapsed session readings for shared frame requests",
	"[studio][pcx_observations]"
) {
	studio::detail::ImageGraphObservations observations;
	std::tm calendar{};
	calendar.tm_year = 126;
	calendar.tm_mon = 9;
	calendar.tm_mday = 2;
	calendar.tm_wday = 5;
	calendar.tm_hour = 15;
	calendar.tm_min = 42;
	calendar.tm_sec = 10;
	const FrameTime frame{7};
	observations.Capture(4, frame, "fixture", 123.25, calendar);
	CHECK(observations.Matches(4, frame, "fixture"));
	CHECK_FALSE(observations.Matches(5, frame, "fixture"));
	CHECK_FALSE(observations.Matches(4, FrameTime{8}, "fixture"));
	EvaluationRequest first, replay;
	observations.Bind(first);
	observations.Bind(replay);
	CHECK(first.ProjectName == "fixture");
	CHECK(first.PcxObservations.data() == replay.PcxObservations.data());
	CHECK(std::get<double>(first.PcxObservations[0].Data) == 123.25);
	CHECK(first.PcxObservations[5].Port == "Device.timeDayInWeek");
	CHECK(std::get<double>(first.PcxObservations[5].Data) == 5);
	CHECK(std::get<double>(first.PcxObservations[6].Data) == 10);
	CHECK(std::get<double>(first.PcxObservations[7].Data) == 2026);
}

TEST_CASE(
	"Source static property edits update the original animator key and undo atomically",
	"[studio][source_inputs]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"node", "pc.number_simple", {}, {}, {{"value", 2.0}}}};
	document.Nodes[0].SourceStaticInputs = {"value"};
	document.Keyframes = {{"node", "value", 0, 2.0, "source", KeyframeEase{}}};
	document.Tracks = {{"node", "value", "wrap", -1}};
	document.Outputs = {{"result", "node", "number"}};
	Diagnostic error;
	const auto sample = [&]() {
		Plan plan;
		const auto compileStatus4 = Compile(document, plan, error);
		INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(compileStatus4 == Status::Ok);
		EvaluationRequest request;
		request.Tick = 17;
		EvaluatedValue value;
		REQUIRE(EvaluateValue(document, plan, "result", request, value, error) == Status::Ok);
		return value.Data;
	};
	document.Nodes[0].Values[0].Data = 99.0;
	CHECK(std::get<double>(sample()) == 2.0);
	document.Nodes[0].Values[0].Data = 2.0;
	const auto before = document;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphHistory history;
	Value replacement = 9.0;
	GroupRefreshEvent edit;
	edit.NodeId = "node";
	edit.EditedPort = "value";
	edit.LocalValue = &replacement;
	edit.At.Tick = 17;
	REQUIRE(host.Edit(document, history, 1, edit, error));
	CHECK(std::get<double>(document.Keyframes[0].Data) == 9.0);
	CHECK(document.Keyframes[0].Tick == 0);
	CHECK(std::get<double>(sample()) == 9.0);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	CHECK(std::get<double>(sample()) == 2.0);
	REQUIRE(history.Redo(document));
	CHECK(std::get<double>(sample()) == 9.0);
}
