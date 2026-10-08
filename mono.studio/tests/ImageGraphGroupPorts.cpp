#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <studio/ImageGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.group_ports")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.group_boundary")

namespace {
	using namespace engine::imagegraph;

	Document SourceGroup() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{1}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}},
			{"number", "pc.number_simple", "group", {}, {{"value", 7.0}}},
			{"alternate", "pc.number_simple", "group", {}, {{"value", 6.75}}},
			{"output", "pc.group_output", "group", {}, {}},
			{"first", "pc.number_simple", {}, {}, {{"value", 0.0}}},
			{"second", "pc.number_simple", {}, {}, {{"value", 0.0}}}
		};
		Group group{"group", "Group"};
		group.Ports = {
			{"input", "input/parent-value", PortDirection::Input, "input"},
			{"output", "output/parent-value", PortDirection::Output, "output"}
		};
		document.Groups.push_back(std::move(group));
		document.Junctions = {
			{"input/parent-value", "group", ValueType::Any, Value{2.5}},
			{"output/parent-value", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {
			{"input/parent-value", "value", "input", "parent_value"},
			{"input", "value", "number", "value"},
			{"number", "number", "output", "value"},
			{"output", "value", "output/parent-value", "value"},
			{"output/parent-value", "value", "first", "value"},
			{"output/parent-value", "value", "second", "value"}
		};
		document.Outputs = {
			{"input-result", "input", "value"},
			{"number-result", "number", "number"},
			{"output-result", "output", "value"},
			{"first-result", "first", "number"},
			{"second-result", "second", "number"}
		};
		return document;
	}

	void CheckValues(const Document &document, double inputValue, double routedValue) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		for (const auto &output : document.Outputs) {
			EvaluatedValue result;
			const auto evaluated = EvaluateValue(document, plan, output.Id, {}, result, diagnostic);
			INFO(output.Id << ": " << diagnostic.Message);
			REQUIRE(evaluated == Status::Ok);
			REQUIRE(std::holds_alternative<double>(result.Data));
			const bool beforeOutput = output.Id == "input-result" || output.Id == "number-result";
			CHECK(std::get<double>(result.Data) == (beforeOutput ? inputValue : routedValue));
		}
	}

	Document Reopen(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(document), restored, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(restored == document);
		return restored;
	}
}

TEST_CASE("Source group typed wires remain visible and editable after reopening", "[studio][groups]") {
	const auto authored = SourceGroup();
	CheckValues(authored, 2.5, 2.5);
	const auto reopened = Reopen(authored);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	const auto input = ids.ToCanvas.at("input"), number = ids.ToCanvas.at("number"),
			   output = ids.ToCanvas.at("output"), alternate = ids.ToCanvas.at("alternate");
	CHECK(canvas.CanConnect(input, "value", number, "value") == nodegraph::LinkResult::Made);
	CHECK(canvas.CanConnect(number, "number", output, "value") == nodegraph::LinkResult::Made);
	CHECK(canvas.Links().size() == authored.Links.size());
	CHECK(ids.UnmappedLinks.empty());
	REQUIRE(canvas.Disconnect(output, "value"));
	REQUIRE(canvas.Connect(alternate, "number", output, "value") == nodegraph::LinkResult::Made);
	Document edited;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, edited, error));
	CHECK(
		std::find(edited.Links.begin(), edited.Links.end(), Link{"number", "number", "output", "value"}) ==
		edited.Links.end()
	);
	CHECK(edited.Outputs == authored.Outputs);
	CheckValues(edited, 2.5, 6.75);
	CheckValues(Reopen(edited), 2.5, 6.75);
}

TEST_CASE(
	"Ungrouping preserves default input and output fanout through standalone junctions", "[studio][groups]"
) {
	const auto authored = SourceGroup();
	CheckValues(authored, 2.5, 2.5);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	REQUIRE(canvas.Ungroup(ids.GroupsToCanvas.at("group"), false));
	Document ungrouped;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, authored, ids, ungrouped, error));
	CHECK(ungrouped.Groups.empty());
	CHECK(ungrouped.Links == authored.Links);
	CHECK(ungrouped.Outputs == authored.Outputs);
	REQUIRE(ungrouped.Junctions.size() == authored.Junctions.size());
	for (const auto &junction : ungrouped.Junctions)
		CHECK(junction.GroupId.empty());
	CheckValues(ungrouped, 2.5, 2.5);
	const auto reopened = Reopen(ungrouped);
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, resaved, error));
	CHECK(resaved == reopened);
	CheckValues(resaved, 2.5, 2.5);
}

TEST_CASE("Source Any sockets preserve the engine union restrictions", "[studio][groups]") {
	auto document = SourceGroup();
	document.Nodes.push_back({"sampler", "value.sample_noise", "group", {}, {}});
	document.Nodes.push_back({"feedback", "pc.feedback_output", "group", {}, {}});
	Plan plan;
	Diagnostic diagnostic;
	const auto baseline = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(baseline == Status::Ok);
	document.Links.push_back({"input", "value", "sampler", "field"});
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Links.pop_back();
	document.Links.push_back({"input", "value", "feedback", "feedback_loop"});
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Links.pop_back();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("input"), "value", ids.ToCanvas.at("sampler"), "field") !=
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("input"), "value", ids.ToCanvas.at("feedback"), "feedback_loop") !=
		nodegraph::LinkResult::Made
	);
}

TEST_CASE("Ungrouping a child preserves its parent routes and grandchildren", "[studio][groups]") {
	auto authored = SourceGroup();
	authored.Groups.front().ParentId = "parent";
	authored.Groups.push_back({"parent", "Parent"});
	authored.Groups.push_back({"grandchild", "Grandchild", "group"});
	for (auto &node : authored.Nodes)
		if (node.Id == "first" || node.Id == "second") node.GroupId = "parent";
	authored.Nodes.push_back({"grandchild-number", "pc.number_simple", "grandchild", {}, {{"value", 2.5}}});
	authored.Outputs.push_back({"grandchild-result", "grandchild-number", "number"});
	CheckValues(authored, 2.5, 2.5);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	REQUIRE(ids.GroupsToCanvas.contains("parent"));
	REQUIRE(ids.GroupsToCanvas.contains("grandchild"));
	REQUIRE(canvas.Ungroup(ids.GroupsToCanvas.at("group"), false));
	Document ungrouped;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, authored, ids, ungrouped, error));
	REQUIRE(ungrouped.Groups.size() == 2);
	const auto grandchild =
		std::find_if(ungrouped.Groups.begin(), ungrouped.Groups.end(), [](const auto &group) {
			return group.Id == "grandchild";
		});
	REQUIRE(grandchild != ungrouped.Groups.end());
	CHECK(grandchild->ParentId == "parent");
	for (const auto &node : ungrouped.Nodes)
		CHECK(node.GroupId == (node.Id == "grandchild-number" ? "grandchild" : "parent"));
	for (const auto &junction : ungrouped.Junctions)
		CHECK(junction.GroupId == "parent");
	CHECK(ungrouped.Links == authored.Links);
	CHECK(ungrouped.Outputs == authored.Outputs);
	CheckValues(ungrouped, 2.5, 2.5);
	const auto reopened = Reopen(ungrouped);
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, resaved, error));
	CHECK(resaved == reopened);
	CheckValues(resaved, 2.5, 2.5);
}

TEST_CASE("Computed noise coordinates keep concrete types beside sampler unions", "[studio][groups]") {
	auto document = SourceGroup();
	document.Nodes.push_back(
		{"computed",
		 "value.noise_field",
		 "group",
		 {},
		 {{"mode", EnumValue{1}},
		  {"dimension", EnumValue{2}},
		  {"output_type", EnumValue{1}},
		  {"position", Vector2{.25, .5}}}}
	);
	document.Nodes.push_back({"coordinates", "pc.vector2", "group", {}, {{"x", .25}, {"y", .5}}});
	document.Nodes.push_back({"sampler", "value.sample_noise", "group", {}, {}});
	Plan plan;
	Diagnostic diagnostic;
	const auto baseline = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(baseline == Status::Ok);
	document.Links.push_back({"input", "value", "computed", "position"});
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Links.back() = {"number", "number", "computed", "position"};
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Links.back() = {"coordinates", "vector", "computed", "position"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("input"), "value", ids.ToCanvas.at("computed"), "position") !=
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("computed"), "position") !=
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(
			ids.ToCanvas.at("coordinates"), "vector", ids.ToCanvas.at("computed"), "position"
		) == nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("coordinates"), "vector", ids.ToCanvas.at("sampler"), "position") ==
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("sampler"), "position") ==
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("input"), "value", ids.ToCanvas.at("sampler"), "position") !=
		nodegraph::LinkResult::Made
	);
}

TEST_CASE("Source dynamic Any ports preserve typed wiring across array split reopening", "[studio][groups]") {
	Document document;
	document.FormatVersion = 9;
	Node array{"array", "pc.array", {}, {}, {{"type", EnumValue{0}}, {"spread_array", true}}};
	array.DynamicInputs = {
		{"value_0", ValueType::Any, Value{2.0}},
		{"value_1", ValueType::Any, Value{0.0}},
		{"value_2", ValueType::Any, Value{8.0}}
	};
	Node split{"split", "pc.array_split", {}, {}, {}};
	split.DynamicOutputs = {{"val_1", ValueType::Any}, {"val_2", ValueType::Any}};
	Node nativeArray{"native-array", "value.array", {}, {}, {}};
	nativeArray.DynamicInputs = {{"item", ValueType::Any, std::nullopt}};
	document.Nodes = {
		std::move(array),
		std::move(split),
		std::move(nativeArray),
		{"source", "pc.number_simple", {}, {}, {{"value", 5.0}}},
		{"alternate", "pc.number_simple", {}, {}, {{"value", 9.0}}},
		{"capture", "pc.number_simple", {}, {}, {{"value", 0.0}}},
		{"native-scalar",
		 "value.noise_field",
		 {},
		 {},
		 {{"mode", EnumValue{1}},
		  {"dimension", EnumValue{1}},
		  {"output_type", EnumValue{1}},
		  {"position", .5}}}
	};
	document.Links = {
		{"source", "number", "array", "value_1"},
		{"array", "array", "split", "array"},
		{"split", "val_1", "capture", "value"}
	};
	document.Outputs = {{"split-result", "split", "val_1"}, {"capture-result", "capture", "number"}};
	CheckValues(document, 5.0, 5.0);
	const auto reopened = Reopen(document);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	CHECK(canvas.Links().size() == document.Links.size());
	CHECK(ids.UnmappedLinks.empty());
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("source"), "number", ids.ToCanvas.at("array"), "value_1") ==
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("split"), "val_1", ids.ToCanvas.at("capture"), "value") ==
		nodegraph::LinkResult::Made
	);
	CHECK(
		canvas.CanConnect(
			ids.ToCanvas.at("native-scalar"), "value", ids.ToCanvas.at("native-array"), "item"
		) != nodegraph::LinkResult::Made
	);
	Plan plan;
	Diagnostic diagnostic;
	auto nativeMismatch = document;
	nativeMismatch.Links.push_back({"native-scalar", "value", "native-array", "item"});
	CHECK(Compile(nativeMismatch, plan, diagnostic) == Status::TypeMismatch);
	Document unchanged;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, unchanged, error));
	CHECK(unchanged == reopened);
	REQUIRE(canvas.Disconnect(ids.ToCanvas.at("array"), "value_1"));
	REQUIRE(
		canvas.Connect(ids.ToCanvas.at("alternate"), "number", ids.ToCanvas.at("array"), "value_1") ==
		nodegraph::LinkResult::Made
	);
	Document edited;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, edited, error));
	CHECK(edited.Nodes[0].DynamicInputs == document.Nodes[0].DynamicInputs);
	CHECK(edited.Nodes[1].DynamicOutputs == document.Nodes[1].DynamicOutputs);
	CheckValues(edited, 9.0, 9.0);
	const auto persisted = Reopen(edited);
	REQUIRE(studio::LoadImageGraphCanvas(persisted, canvas, ids, error));
	CHECK(ids.UnmappedLinks.empty());
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, persisted, ids, resaved, error));
	CHECK(resaved == persisted);
	CheckValues(resaved, 9.0, 9.0);
}

TEST_CASE("Ungrouping a source surface output keeps native image consumers connected", "[studio][groups]") {
	Document authored;
	authored.FormatVersion = 9;
	authored.Nodes = {
		{"solid",
		 "image.solid",
		 "group",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}}},
		{"output", "pc.group_output", "group", {}, {}},
		{"sink", "image.passthrough", {}, {}, {}}
	};
	Group group{"group", "Group"};
	group.Ports = {{"output", "output/parent-value", PortDirection::Output, "output"}};
	authored.Groups.push_back(std::move(group));
	authored.Junctions = {{"output/parent-value", "group", ValueType::Any, std::nullopt}};
	authored.Links = {
		{"solid", "image", "output", "value"},
		{"output", "value", "output/parent-value", "value"},
		{"output/parent-value", "value", "sink", "image"}
	};
	authored.Outputs = {{"source-result", "output", "value"}, {"sink-result", "sink", "image"}};
	const auto checkPixels = [](const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compiled == Status::Ok);
		for (const auto &output : document.Outputs) {
			Image image;
			const auto evaluated = Evaluate(document, plan, output.Id, image, diagnostic);
			INFO(output.Id << ": " << diagnostic.Message);
			REQUIRE(evaluated == Status::Ok);
			CHECK(image.Width == 1);
			CHECK(image.Height == 1);
			CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
		}
	};
	checkPixels(authored);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("solid"), "image", ids.ToCanvas.at("output"), "value") ==
		nodegraph::LinkResult::Made
	);
	CHECK(canvas.Links().size() == authored.Links.size());
	CHECK(ids.UnmappedLinks.empty());
	REQUIRE(canvas.Ungroup(ids.GroupsToCanvas.at("group"), false));
	Document ungrouped;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, authored, ids, ungrouped, error));
	CHECK(ungrouped.Groups.empty());
	REQUIRE(ungrouped.Junctions.size() == 1);
	CHECK(ungrouped.Junctions.front().GroupId.empty());
	CHECK(ungrouped.Links == authored.Links);
	CHECK(ungrouped.Outputs == authored.Outputs);
	checkPixels(ungrouped);
	const auto reopened = Reopen(ungrouped);
	checkPixels(reopened);
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, resaved, error));
	CHECK(resaved == reopened);
	checkPixels(resaved);
}

TEST_CASE("Group boundary junction routes edit through durable canvas sockets", "[studio][groups]") {
	auto authored = SourceGroup();
	authored.Nodes.push_back({"driver", "pc.number_simple", "", {}, {{"value", 8.5}}});
	authored.Nodes.push_back({"replacement", "pc.number_simple", "", {}, {{"value", 6.75}}});
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	REQUIRE(ids.UnmappedLinks.empty());
	REQUIRE(canvas.Links().size() == authored.Links.size());
	const auto parentInput = ids.ToCanvas.at("input/parent-value");
	const auto parentOutput = ids.ToCanvas.at("output/parent-value");
	CHECK(canvas.GroupOf(parentInput) == ids.GroupsToCanvas.at("group"));
	CHECK(canvas.GroupOf(parentOutput) == ids.GroupsToCanvas.at("group"));
	CHECK(canvas.Find(parentInput)->Label == "Group input: input");
	CHECK(canvas.Find(parentOutput)->Label == "Group output: output");
	REQUIRE(
		canvas.Connect(ids.ToCanvas.at("driver"), "number", parentInput, "value") ==
		nodegraph::LinkResult::Made
	);
	REQUIRE(canvas.Disconnect(ids.ToCanvas.at("first"), "value"));
	REQUIRE(
		canvas.Connect(ids.ToCanvas.at("replacement"), "number", ids.ToCanvas.at("first"), "value") ==
		nodegraph::LinkResult::Made
	);
	Document edited;
	const bool saved = studio::SaveImageGraphCanvas(canvas, authored, ids, edited, error);
	INFO(error);
	REQUIRE(saved);
	CHECK(edited.Nodes.size() == authored.Nodes.size());
	CHECK(edited.Junctions == authored.Junctions);
	CHECK(edited.Groups == authored.Groups);
	CHECK(std::none_of(edited.Nodes.begin(), edited.Nodes.end(), [](const auto &node) {
		return node.Type == "studio.imagegraph.junction";
	}));
	CHECK(
		std::find(
			edited.Links.begin(), edited.Links.end(), Link{"driver", "number", "input/parent-value", "value"}
		) != edited.Links.end()
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(edited, plan, diagnostic) == Status::Ok);
	for (const auto &output : edited.Outputs) {
		EvaluatedValue value;
		REQUIRE(EvaluateValue(edited, plan, output.Id, {}, value, diagnostic) == Status::Ok);
		CHECK(value.Data == Value{output.Id == "first-result" ? 6.75 : 8.5});
	}
	const auto reopened = Reopen(edited);
	REQUIRE(studio::LoadImageGraphCanvas(reopened, canvas, ids, error));
	CHECK(ids.UnmappedLinks.empty());
	CHECK(canvas.Links().size() == reopened.Links.size());
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, reopened, ids, resaved, error));
	CHECK(resaved == reopened);
}

TEST_CASE("Invalid boundary canvas edits refuse without publishing authored changes", "[studio][groups]") {
	const auto authored = SourceGroup();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	const auto endpointIds = ids.ToCanvas;
	const auto reverseIds = ids.ToDocument;
	Document destination = authored;
	bool bypassedInterface = false;
	SECTION("Required junction deletion") {
		canvas.Remove(ids.ToCanvas.at("input/parent-value"));
	}
	SECTION("Cycle attached directly to a boundary") {
		canvas.Attach({ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("input/parent-value"), "value"});
	}
	SECTION("Child producer bypasses its public output socket") {
		bypassedInterface = true;
		REQUIRE(canvas.Disconnect(ids.ToCanvas.at("first"), "value"));
		canvas.Attach({ids.ToCanvas.at("alternate"), "number", ids.ToCanvas.at("first"), "value"});
	}
	const bool saved = studio::SaveImageGraphCanvas(canvas, authored, ids, destination, error);
	INFO(error);
	CHECK_FALSE(saved);
	if (bypassedInterface)
		CHECK(error == "link crosses a subgraph without an interface port node=first port=value");
	CHECK_FALSE(error.empty());
	CHECK(destination == authored);
	CHECK(ids.ToCanvas == endpointIds);
	CHECK(ids.ToDocument == reverseIds);
}

TEST_CASE("Malformed junction types refuse canvas projection", "[studio][groups]") {
	auto authored = SourceGroup();
	authored.Junctions.front().Type = static_cast<ValueType>(255);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	CHECK_FALSE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	CHECK_FALSE(error.empty());
	CHECK(canvas.Nodes().empty());
	CHECK(ids.ToCanvas.empty());
	CHECK(ids.ToDocument.empty());
}
