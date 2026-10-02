#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/ImageGraphPorts.hpp"
#include "../src/ImageGraphPreview.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Types.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.outputs")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("studio.nodegraph.graph")
using namespace engine::imagegraph;

TEST_CASE(
	"Array Split output authoring links selected ports and preserves one undo across resize",
	"[studio][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"split", "pc.array_split", "group", {}, {}},
		{"sink", "pc.array_copy", "group", {}, {}},
		{"source",
		 "pc.array",
		 "group",
		 {},
		 {{"type", EnumValue{0}}, {"spread_array", true}},
		 {{"value_0", ValueType::Array, Value{ArrayValue{ValueType::Scalar, {2.0, 4.0, 8.0}}}}}}
	};
	document.Groups = {{"group", "Group"}};
	document.Outputs = {{"result", "split", "val_0"}};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphSplitOutputCount(document, "split", 3, error));
	REQUIRE(document.Nodes[0].DynamicOutputs.size() == 2);
	document.Links = {{"source", "array", "split", "array"}, {"split", "val_2", "sink", "array"}};
	REQUIRE(studio::SetImageGraphOutput(document, "result", "split", "val_2", error));
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, failure));
	REQUIRE(
		canvas.CanConnect(ids.ToCanvas.at("split"), "val_1", ids.ToCanvas.at("sink"), "array") ==
		nodegraph::LinkResult::Made
	);
	Document restored;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, restored, failure));
	CHECK(restored == document);
	Plan plan;
	const auto restoredStatus = Compile(restored, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(restoredStatus == Status::Ok);
	studio::ImageGraphPreviewValue preview;
	REQUIRE(studio::EvaluateImageGraphPreview(restored, plan, "result", {}, preview, error) == Status::Ok);
	CHECK(std::get<double>(std::get<EvaluatedValue>(preview).Data) == 8.0);
	studio::ImageGraphHistory history;
	const auto before = document;
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		return studio::SetImageGraphSplitOutputCount(staged, "split", 1, error);
	}));
	CHECK(document.Nodes[0].DynamicOutputs.empty());
	CHECK(document.Links == std::vector<Link>{{"source", "array", "split", "array"}});
	CHECK(document.Outputs.empty());
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(document.Nodes[0].DynamicOutputs.empty());
	const auto prior = document;
	CHECK_FALSE(studio::SetImageGraphSplitOutputCount(document, "split", 4097, error));
	CHECK(document == prior);
}

TEST_CASE(
	"channel array mode is an instance output interface and survives canvas round trip",
	"[studio][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"array", "pc.color_to_rgb", "", {}, {{"output_array", true}}},
		{"channels", "pc.color_to_rgb", "", {}, {{"output_array", false}}}
	};
	document.Outputs = {{"result", "array", "red"}};
	const auto ports = studio::detail::ImageGraphOutputPorts(document.Nodes[0]);
	REQUIRE(ports.size() == 1);
	CHECK(ports[0].Type == ValueType::Array);
	CHECK(studio::detail::ImageGraphOutputPorts(document.Nodes[1]).size() > 1);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, failure));
	REQUIRE(canvas.Find(ids.ToCanvas.at("array"))->OutputPorts);
	CHECK(canvas.Find(ids.ToCanvas.at("array"))->OutputPorts->size() == 1);
	Document restored;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, restored, failure));
	CHECK(restored == document);
	Diagnostic error;
	CHECK_FALSE(studio::SetImageGraphOutput(document, "result", "array", "green", error));
	CHECK(studio::SetImageGraphOutput(document, "result", "channels", "green", error));
}

TEST_CASE(
	"source group sockets use the imported constructor and one undo transaction",
	"[studio][source_group_ports]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Groups = {{"group", "Group"}};
	studio::ImageGraphHistory history;
	Diagnostic error;
	const auto before = document;
	const bool accepted = studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		if (!studio::AddSourceImageGraphGroupPort(staged, "group", "input", PortDirection::Input, error) ||
			!studio::AddSourceImageGraphGroupPort(staged, "group", "output", PortDirection::Output, error))
			return false;
		staged.Outputs = {{"result", "input", "value"}};
		return true;
	});
	INFO(error.Message);
	REQUIRE(accepted);
	REQUIRE(document.Nodes.size() == 2);
	REQUIRE(document.Groups[0].Ports.size() == 2);
	CHECK(document.Groups[0].Ports[0].ControlNodeId == "input");
	CHECK(document.Groups[0].Ports[0].JunctionId == "input/parent-value");
	CHECK(document.Junctions[0].Type == ValueType::Any);
	Plan plan;
	const auto compileStatus1 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus1 == Status::Ok);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(document));
	CHECK(document.Nodes.size() == 2);
}

TEST_CASE("output transactions reject partial edits and missing undo budget", "[studio][dynamic_outputs]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"split", "pc.array_split", "", {}, {}}};
	const auto before = document;
	Diagnostic error;
	studio::ImageGraphHistory history;
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		REQUIRE(studio::SetImageGraphSplitOutputCount(staged, "split", 3, error));
		return false;
	}));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	studio::ImageGraphHistory tiny(4, 1);
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(document, tiny, [&](Document &staged) {
		return studio::SetImageGraphSplitOutputCount(staged, "split", 3, error);
	}));
	CHECK(document == before);
	CHECK_FALSE(tiny.CanUndo());
}

TEST_CASE("owned path and pixel box types register durable canvas sockets", "[studio][dynamic_outputs]") {
	studio::RegisterImageGraphNodeTypes();
	for (const auto type : {ValueType::Path3D, ValueType::PixelBox}) {
		const auto name = std::string(ValueTypeName(type));
		CHECK(nodegraph::DataTypes::Find("imagegraph." + name) != nullptr);
		CHECK(ParseValueTypeName(name) == type);
		CHECK(studio::detail::ImageGraphValuePreviewSupported(type));
	}
}

TEST_CASE("rejected oversized edit retains redo and admitted growth can undo", "[studio][dynamic_outputs]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"split", "pc.array_split", "", {}, {}}};
	const auto before = document;
	Document grown = before;
	Diagnostic error;
	REQUIRE(studio::SetImageGraphSplitOutputCount(grown, "split", 8, error));
	const auto capacity = std::max(Write(before).size(), Write(grown).size());
	studio::ImageGraphHistory history(4, capacity);
	REQUIRE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		staged = grown;
	}));
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.CanRedo());
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
		return studio::SetImageGraphSplitOutputCount(staged, "split", 64, error);
	}));
	CHECK(document == before);
	REQUIRE(history.CanRedo());
	REQUIRE(history.Redo(document));
	CHECK(document == grown);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
}

TEST_CASE(
	"Array Split canvas admits outputs past the generic dynamic input bound", "[studio][dynamic_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"split", "pc.array_split", "", {}, {}}};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphSplitOutputCount(document, "split", 65, error));
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(document, graph, ids, failure));
	REQUIRE(graph.Find(ids.ToCanvas.at("split"))->OutputPorts);
	CHECK(graph.Find(ids.ToCanvas.at("split"))->OutputPorts->size() == 65);
	Document restored;
	REQUIRE(studio::SaveImageGraphCanvas(graph, document, ids, restored, failure));
	CHECK(restored == document);
}
