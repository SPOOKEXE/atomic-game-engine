#include "../src/ImageGraphSourceEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.source_edit")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.group_replay")

using namespace engine::imagegraph;

TEST_CASE(
	"Source input undo admission preserves frozen Group declarations atomically",
	"[studio][source_transaction]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{11};
	document.Nodes = {
		{"input",
		 "pc.group_input",
		 "group",
		 {},
		 {{"input_type", EnumValue{2}},
		  {"subtype", EnumValue{0}},
		  {"vector_size", EnumValue{0}},
		  {"parent_value", .5}}},
		{"output", "pc.group_output", "group", {}, {}},
		{"lua", "pc.lua_compute", {}, {}, {}}
	};
	document.Nodes[0].SourceAnimatedInputs = {"input_type"};
	document.Keyframes = {{"input", "input_type", 0, EnumValue{2}, "source", KeyframeEase{}}};
	document.Tracks = {{"input", "input_type", "wrap", -1}};
	document.Groups = {
		{"group",
		 "Group",
		 {},
		 {{"input", "in", PortDirection::Input, "input"}, {"output", "out", PortDirection::Output, "output"}}}
	};
	document.Junctions = {
		{"in", "group", ValueType::Any, .5}, {"out", "group", ValueType::Any, std::nullopt}
	};
	document.Links = {{"input", "value", "output", "value"}, {"output", "value", "out", "value"}};
	document.Outputs = {{"result", "output", "value"}};
	Diagnostic error;
	REQUIRE(studio::SetSourceImageGraphDynamicGroupCount(document, "lua", 1, error));
	studio::ImageGraphHistory history;
	studio::ImageGraphGroupHost host;
	Value floatKind = EnumValue{1};
	GroupRefreshEvent freeze;
	freeze.NodeId = "input";
	freeze.EditedPort = "input_type";
	freeze.LocalValue = &floatKind;
	freeze.LocalAnimated = true;
	freeze.At.Tick = 5;
	const bool frozen = host.Edit(document, history, 1, freeze, error, false);
	INFO(error.Message);
	REQUIRE(frozen);
	REQUIRE(host.Replay.Find("input"));
	CHECK(host.Replay.Find("input")->Domain.Kind == SourceSocketKind::Float);
	CHECK(std::get<EnumValue>(document.Nodes[0].Values[0].Data).Value == 2);
	const auto before = document;
	const auto domain = host.Replay.Find("input")->Domain;
	const uint64_t retained = host.Replay.RetainedBytes();
	const auto revision = host.Revision;
	auto replacement = document.Nodes[2].DynamicInputs.back();
	REQUIRE(replacement.Id == "argument_value_0");
	replacement.Default = 7.5;
	EvaluationRequest request;
	request.Tick = 5;
	studio::ImageGraphHistory refused(0);
	CHECK_FALSE(
		studio::ApplyImageGraphSourceDynamicInput(
			document, refused, host, 2, "lua", replacement, request, error
		)
	);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(document == before);
	CHECK(host.Revision == revision);
	CHECK(host.Replay.RetainedBytes() == retained);
	CHECK(host.Replay.Find("input")->Domain == domain);
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(refused.CanUndo());
	const bool accepted = studio::ApplyImageGraphSourceDynamicInput(
		document, history, host, 2, "lua", replacement, request, error
	);
	INFO(error.Message);
	REQUIRE(accepted);
	CHECK(host.BorrowedBytes == 0);
	CHECK(host.Replay.Find("input")->Domain == domain);
	CHECK(document != before);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(std::get<double>(document.Nodes[2].DynamicInputs.back().Default.value()) == 7.5);
}
