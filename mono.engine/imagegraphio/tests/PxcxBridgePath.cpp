#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_bridge_path")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}, {"future_input", "keep"}};
	}
	Json Wire(std::string node, int output = 0) {
		return {{"from_node", std::move(node)}, {"from_index", output}, {"from_tag", 0}};
	}
	Json Record(std::string id, const char *type, Json inputs) {
		return {
			{"id", std::move(id)},
			{"type", type},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Line(const char *id, double y) {
		return Record(
			id,
			"Node_Path_Smooth",
			Json::array(
				{Fixed(false),
				 Fixed(false),
				 Fixed(3),
				 Fixed(0),
				 Fixed(0),
				 Fixed(true),
				 Fixed(Json::array({0, y})),
				 Fixed(Json::array({32, y}))}
			)
		);
	}
	Json Project(bool grouped = false) {
		Json bridge = Json::array(
			{Wire("first"),
			 Fixed(3),
			 Fixed(false),
			 Fixed(0),
			 Fixed(17),
			 Fixed(0),
			 Fixed(Json::array({0, 1})),
			 Fixed(false),
			 Wire("second"),
			 Wire("third")}
		);
		Json nodes = Json::array(
			{Line("first", 0),
			 Line("second", 8),
			 Line("third", 20),
			 Record("bridge", "Node_Path_Bridge", bridge)}
		);
		for (int index = 0; index < 2; ++index) {
			nodes.push_back(Record(
				"sample" + std::to_string(index),
				"Node_Path_Sample",
				Json::array(
					{Wire("bridge"), Fixed(.5), Fixed(2), Fixed(Json::array({0, 1})), Fixed(0), Fixed(index)}
				)
			));
		}
		if (grouped) {
			for (auto &node : nodes)
				node["group"] = "group";
			auto group = Record("group", "Node_Group", Json::array());
			group["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}};
			nodes.push_back(std::move(group));
		}
		return {
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", Json::array({1, "keep"})},
			{"nodes", std::move(nodes)}
		};
	}
	PxcxImport Reopen(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		return imported;
	}
	PxcxImport Import(const Json &project) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		return Reopen(bytes);
	}
	Node &Native(Document &document, std::string_view id) {
		auto found = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == id;
		});
		REQUIRE(found != document.Nodes.end());
		return *found;
	}
	const Value &Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return value.Data;
		FAIL("missing Bridge control: " << port);
		return node.Values.front().Data;
	}
	std::array<Value, 3> Evaluate(Document document) {
		document.Outputs = {
			{"path", "bridge", "path"}, {"sample0", "sample0", "position"}, {"sample1", "sample1", "position"}
		};
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		SourceBuiltinRandomCapture capture;
		const auto &bridge = Native(document, "bridge");
		if (std::get<EnumValue>(Authored(bridge, "distribution")).Value == 1) {
			status =
				PrepareSourceBuiltinRandomCapture(document, plan, "bridge", request, capture, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			// Injected observations test the capture adapter, not the desktop seed algorithm.
			capture.Draws = {
				{SourceBuiltinRandomOperation::Random, 0, 1, .2},
				{SourceBuiltinRandomOperation::Random, 0, 1, .8}
			};
			request.BuiltinRandomCaptures = {&capture, 1};
		}
		std::array<Value, 3> results;
		for (size_t index = 0; index < results.size(); ++index) {
			EvaluatedValue output;
			status = EvaluateValue(document, plan, document.Outputs[index].Id, request, output, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			results[index] = std::move(output.Data);
		}
		return results;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	PxcxImport Save(const PxcxImport &imported, const Document &desired) {
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		return Reopen(bytes);
	}
}

TEST_CASE(
	"PXC Bridge saves seven controls and ordered dynamic paths with downstream fanout", "[pxcx_bridge_path]"
) {
	const auto project = Project();
	auto imported = Import(project);
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const auto &bridge = Native(imported.Graph, "bridge");
	REQUIRE(bridge.Type == "pc.path_bridge");
	REQUIRE(bridge.DynamicInputs.size() == 2);
	CHECK(bridge.DynamicInputs[0].Id == "path_0");
	CHECK(bridge.DynamicInputs[1].Id == "path_1");
	for (const auto &input : bridge.DynamicInputs) {
		CHECK(input.Type == ValueType::Path2D);
		CHECK_FALSE(input.Default);
	}
	const std::array<AuthoredValue, 7> controls{
		{{"amount", int64_t{3}},
		 {"smooth", false},
		 {"distribution", EnumValue{0}},
		 {"seed", 17.0},
		 {"offset", 0.0},
		 {"range", Vector2{0, 1}},
		 {"loop", false}}
	};
	for (const auto &control : controls)
		CHECK(Authored(bridge, control.Port) == control.Data);
	const auto before = Evaluate(imported.Graph);
	CHECK(before[1] == Value{Vector2{0, 10}});
	CHECK(before[2] == Value{Vector2{16, 10}});
	const std::array<PxcxEdit, 7> edits{
		PxcxInputValueEdit{"bridge", "amount", int64_t{2}},
		PxcxInputValueEdit{"bridge", "smooth", true},
		PxcxInputValueEdit{"bridge", "distribution", EnumValue{1}},
		PxcxInputValueEdit{"bridge", "seed", 23.0},
		PxcxInputValueEdit{"bridge", "offset", .125},
		PxcxInputValueEdit{"bridge", "range", Vector2{.2, .8}},
		PxcxInputValueEdit{"bridge", "loop", true}
	};
	std::vector<std::byte> editedBytes;
	Diagnostic diagnostic;
	const bool edited = WritePxcxEdits(imported, originalBytes, edits, editedBytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(edited);
	auto desired = Reopen(editedBytes).Graph;
	const auto expected = Evaluate(desired);
	CHECK(expected[1] != before[1]);
	Document nativeReload;
	REQUIRE(Read(Write(desired), nativeReload, diagnostic) == Status::Ok);
	CHECK(nativeReload == desired);
	CHECK(Evaluate(nativeReload) == expected);
	Document receipt;
	receipt.FormatVersion = 9;
	for (int64_t index = 0; index < 2; ++index) {
		const auto id = "sample" + std::to_string(index);
		receipt.Nodes.push_back(
			{id,
			 "pc.path_sample",
			 "",
			 {},
			 {{"path", expected[0]}, {"ratio", .5}, {"type", EnumValue{2}}, {"path_index", index}}}
		);
		receipt.Outputs.push_back({id, id, "position"});
	}
	Document durable;
	REQUIRE(Read(Write(receipt), durable, diagnostic) == Status::Ok);
	CHECK(durable == receipt);
	Plan durablePlan;
	REQUIRE(Compile(durable, durablePlan, diagnostic) == Status::Ok);
	for (size_t index = 0; index < durable.Outputs.size(); ++index) {
		EvaluatedValue output;
		const auto status =
			EvaluateValue(durable, durablePlan, durable.Outputs[index].Id, {}, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Data == expected[index + 1]);
	}
	auto reopened = Save(imported, desired);
	CHECK(Evaluate(reopened.Graph) == expected);
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Native(reopened.Graph, "bridge").DynamicInputs == bridge.DynamicInputs);
	for (const auto &edit : edits) {
		const auto &control = std::get<PxcxInputValueEdit>(edit);
		CHECK(Authored(Native(reopened.Graph, "bridge"), control.Port) == control.Data);
	}
	const auto json = Source(reopened);
	CHECK(json["future_project"] == project["future_project"]);
	for (size_t index : {0, 1, 2, 4, 5})
		CHECK(json["nodes"][index] == project["nodes"][index]);
	CHECK(json["nodes"][3]["future_node"] == "keep");
	CHECK(json["nodes"][3]["inputs"][8] == project["nodes"][3]["inputs"][8]);
	CHECK(json["nodes"][3]["inputs"][9] == project["nodes"][3]["inputs"][9]);
	auto unchanged = Save(reopened, reopened.Graph);
	CHECK(unchanged.Source.OriginalBytes == reopened.Source.OriginalBytes);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE("PXC Bridge grouped dynamic paths keep source order when links change", "[pxcx_bridge_path]") {
	auto imported = Import(Project(true));
	REQUIRE(Native(imported.Graph, "bridge").Type == "pc.path_bridge");
	CHECK(Native(imported.Graph, "bridge").GroupId == "group");
	const auto before = Evaluate(imported.Graph);
	auto desired = imported.Graph;
	for (auto &link : desired.Links) {
		if (link.ToNode != "bridge") continue;
		if (link.ToPort == "path_0")
			link.FromNode = "third";
		else if (link.ToPort == "path_1")
			link.FromNode = "second";
	}
	const auto expected = Evaluate(desired);
	CHECK(expected[1] == Value{Vector2{0, 16}});
	CHECK(expected[1] != before[1]);
	CHECK(expected[0] != before[0]);
	auto reopened = Save(imported, desired);
	CHECK(Native(reopened.Graph, "bridge").GroupId == "group");
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Evaluate(reopened.Graph) == expected);
	CHECK(Source(reopened)["nodes"][6] == Source(imported)["nodes"][6]);
	CHECK(Source(reopened)["nodes"][3]["inputs"][8]["from_node"] == "third");
	CHECK(Source(reopened)["nodes"][3]["inputs"][9]["from_node"] == "second");
}

TEST_CASE("PXC Bridge dynamic path bypass survives native and source graph saves", "[pxcx_bridge_path]") {
	auto project = Project();
	project["nodes"][3]["inputs"][8]["bypass"] = true;
	project["nodes"][4]["inputs"][0] = Wire("bridge", 1008);
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "bridge").Type == "pc.path_bridge");
	const auto expected = Evaluate(imported.Graph);
	CHECK(expected[1] == Value{Vector2{16, 8}});
	auto desired = imported.Graph;
	for (auto &link : desired.Links)
		if (link.ToNode == "bridge" && link.ToPort == "path_0") link.FromNode = "third";
	const auto edited = Evaluate(desired);
	CHECK(edited[1] == Value{Vector2{16, 20}});
	Diagnostic diagnostic;
	Document nativeReload;
	REQUIRE(Read(Write(desired), nativeReload, diagnostic) == Status::Ok);
	CHECK(Evaluate(nativeReload) == edited);
	auto reopened = Save(imported, desired);
	CHECK(Evaluate(reopened.Graph) == edited);
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Source(reopened)["nodes"][4] == project["nodes"][4]);
	CHECK(Source(reopened)["nodes"][3]["inputs"][8]["bypass"] == true);
}

TEST_CASE("PXC Bridge missing dynamic source endpoints remain opaque", "[pxcx_bridge_path]") {
	auto project = Project();
	SECTION("Undeclared bypass") {
		project["nodes"][4]["inputs"][0] = Wire("bridge", 1010);
	}
	SECTION("Unknown output") {
		project["nodes"][4]["inputs"][0] = Wire("bridge", 1);
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "bridge").Type == "pxcx.opaque/Node_Path_Bridge");
	CHECK_FALSE(imported.Diagnostics.empty());
	const auto retained = Save(imported, imported.Graph);
	CHECK(retained.Source.OriginalBytes == imported.Source.OriginalBytes);
}

TEST_CASE("PXC Bridge absent dynamic paths retain empty constructor slots", "[pxcx_bridge_path]") {
	const auto original = Import(Project());
	auto project = Project();
	project["nodes"][3]["inputs"].push_back(Fixed(-4));
	auto imported = Import(project);
	const auto &bridge = Native(imported.Graph, "bridge");
	REQUIRE(bridge.Type == "pc.path_bridge");
	REQUIRE(bridge.DynamicInputs.size() == 3);
	CHECK(bridge.DynamicInputs[2].Id == "path_2");
	CHECK_FALSE(bridge.DynamicInputs[2].Default);
	const auto expected = Evaluate(original.Graph);
	const auto actual = Evaluate(imported.Graph);
	CHECK(actual[1] == expected[1]);
	CHECK(actual[2] == expected[2]);
	const auto reopened = Save(imported, imported.Graph);
	CHECK(reopened.Source.OriginalBytes == imported.Source.OriginalBytes);
	CHECK(Evaluate(reopened.Graph) == actual);
}

TEST_CASE("PXC Bridge appends an absent path slot without creating a path object", "[pxcx_bridge_path]") {
	auto imported = Import(Project());
	auto desired = imported.Graph;
	Native(desired, "bridge").DynamicInputs.push_back({"path_2", ValueType::Path2D, std::nullopt});
	const auto expected = Evaluate(imported.Graph);
	CHECK(Evaluate(desired) == expected);
	auto reopened = Save(imported, desired);
	const auto &bridge = Native(reopened.Graph, "bridge");
	REQUIRE(bridge.DynamicInputs.size() == 3);
	CHECK(bridge.DynamicInputs.back().Id == "path_2");
	CHECK_FALSE(bridge.DynamicInputs.back().Default);
	CHECK(
		std::find(bridge.SourceStaticInputs.begin(), bridge.SourceStaticInputs.end(), "path_2") !=
		bridge.SourceStaticInputs.end()
	);
	CHECK(Evaluate(reopened.Graph) == expected);
	CHECK(Source(reopened)["nodes"][3]["inputs"][10] == Json::object());
	const auto unchanged = Save(reopened, reopened.Graph);
	CHECK(unchanged.Source.OriginalBytes == reopened.Source.OriginalBytes);
}

TEST_CASE(
	"PXC refuses explicitly authored empty path objects without replacing source", "[pxcx_bridge_path]"
) {
	auto imported = Import(Project());
	const auto original = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	auto desired = original;
	std::string port;
	bool directEdit = false;
	SECTION("Fixed input native object") {
		port = "path";
		Native(desired, "bridge").Values.push_back({port, Path2D{}});
	}
	SECTION("Dynamic input native object") {
		port = "path_0";
		Native(desired, "bridge").DynamicInputs.front().Default = Path2D{};
	}
	SECTION("Explicit input edit") {
		port = "path";
		directEdit = true;
	}
	const std::vector<std::byte> sentinel{std::byte{0x41}};
	auto bytes = sentinel;
	Diagnostic diagnostic;
	if (directEdit) {
		const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"bridge", port, Path2D{}}};
		CHECK_FALSE(WritePxcxEdits(imported, originalBytes, edits, bytes, diagnostic));
	} else {
		Document nativeReload;
		REQUIRE(Read(Write(desired), nativeReload, diagnostic) == Status::Ok);
		CHECK(nativeReload == desired);
		CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	}
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "bridge");
	CHECK(diagnostic.Port == port);
	CHECK(
		diagnostic.Message ==
		"PXC cannot serialize an authored path object; save the native document or connect a source path node"
	);
	CHECK(bytes == sentinel);
	CHECK(imported.Graph == original);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}
