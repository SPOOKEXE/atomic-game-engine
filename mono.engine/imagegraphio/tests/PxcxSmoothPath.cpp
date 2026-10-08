#include <engine/imagegraph/Catalogue.hpp>
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

TEST_SUITE_ID("engine.imagegraphio.pxcx_smooth_path")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;

	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}, {"future_input", "keep"}};
	}
	Json Wire(const char *node) {
		return {{"from_node", node}, {"from_index", 0}, {"from_tag", 0}};
	}
	Json Record(const char *id, const char *type, Json inputs) {
		return {
			{"id", id},
			{"type", type},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Project() {
		Json smooth = Json::array(
			{Fixed(false),
			 Fixed(false),
			 Fixed(3),
			 Fixed(.5),
			 Fixed(0),
			 Fixed(true),
			 Fixed(Json::array({0, 0})),
			 Fixed(Json::array({32, 0}))}
		);
		Json sample = Json::array(
			{Wire("smooth"), Fixed(.5), Fixed(2), Fixed(Json::array({0, 1})), Fixed(0), Fixed(0)}
		);
		return {
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", Json::array({1, "keep"})},
			{"nodes",
			 Json::array(
				 {Record("smooth", "Node_Path_Smooth", smooth),
				  Record("sample", "Node_Path_Sample", sample),
				  Record("length", "Node_Path_Length", Json::array({Wire("smooth")}))}
			 )}
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
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
			return candidate.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const Value &Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return value.Data;
		FAIL("missing Smooth control: " << port);
		return node.Values.front().Data;
	}
	std::array<Value, 4> Evaluate(Document document) {
		document.Outputs = {
			{"path", "smooth", "path_data"},
			{"direct", "smooth", "position_out"},
			{"sample", "sample", "position"},
			{"length", "length", "surface_out"}
		};
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		std::array<Value, 4> values;
		for (size_t index = 0; index < values.size(); ++index) {
			EvaluatedValue output;
			status = EvaluateValue(document, plan, document.Outputs[index].Id, {}, output, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			values[index] = std::move(output.Data);
		}
		return values;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	void SetStatic(Document &document, const char *nodeId, std::string_view port, Value value) {
		auto &node = Native(document, nodeId);
		auto authored = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &candidate) {
			return candidate.Port == port;
		});
		if (authored == node.Values.end())
			node.Values.push_back({std::string(port), value});
		else
			authored->Data = value;
		for (auto &key : document.Keyframes)
			if (key.NodeId == nodeId && key.Port == port) key.Data = value;
	}
}

TEST_CASE("PXC Smooth maps and saves six controls and dynamic anchors", "[pxcx_smooth_path]") {
	const auto project = Project();
	auto imported = Import(project);
	auto &smooth = Native(imported.Graph, "smooth");
	REQUIRE(smooth.Type == "pc.path_smooth");
	const std::array<AuthoredValue, 6> controls{
		{{"loop", false},
		 {"round_anchor", false},
		 {"smoothness", 3.0},
		 {"sample_path", .5},
		 {"sample_mode", EnumValue{0}},
		 {"normalized_length", true}}
	};
	for (const auto &control : controls)
		CHECK(Authored(smooth, control.Port) == control.Data);
	REQUIRE(smooth.DynamicInputs.size() == 2);
	CHECK(smooth.DynamicInputs[0].Type == ValueType::Vector2);
	CHECK(smooth.DynamicInputs[0].Default == std::optional<Value>{Vector2{0, 0}});
	CHECK(smooth.DynamicInputs[1].Default == std::optional<Value>{Vector2{32, 0}});
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const auto before = Evaluate(imported.Graph);
	CHECK(before[1] == Value{Vector2{16, 0}});
	CHECK(before[2] == Value{Vector2{16, 0}});
	CHECK(before[3] == Value{31.0});
	const std::array<PxcxEdit, 6> edits{
		PxcxInputValueEdit{"smooth", "loop", true},
		PxcxInputValueEdit{"smooth", "round_anchor", true},
		PxcxInputValueEdit{"smooth", "smoothness", 4.0},
		PxcxInputValueEdit{"smooth", "sample_path", .25},
		PxcxInputValueEdit{"smooth", "sample_mode", EnumValue{1}},
		PxcxInputValueEdit{"smooth", "normalized_length", false}
	};
	std::vector<std::byte> controlBytes;
	Diagnostic diagnostic;
	bool saved = WritePxcxEdits(imported, originalBytes, edits, controlBytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	auto desired = Reopen(controlBytes).Graph;
	auto &edited = Native(desired, "smooth");
	const std::array<Vector2, 2> anchors{{{.25, .25}, {48.25, .25}}};
	for (size_t index = 0; index < anchors.size(); ++index) {
		auto &input = edited.DynamicInputs[index];
		input.Default = anchors[index];
		for (auto &key : desired.Keyframes)
			if (key.NodeId == "smooth" && key.Port == input.Id) key.Data = anchors[index];
	}
	const auto expected = Evaluate(desired);
	CHECK(expected[1] == Value{Vector2{12, 0}});
	CHECK(expected[1] != before[1]);
	const auto &path = std::get<Path2D>(expected[0]);
	REQUIRE(path.SourceSmooth);
	CHECK_FALSE(path.SourceSmooth->NormalizedLength);
	CHECK(path.Loop);
	REQUIRE(path.Anchors.size() == 2);
	CHECK(path.Anchors[0].Controls[0] == 0);
	CHECK(path.Anchors[1].Controls[0] == 48);
	Document nativeReload;
	REQUIRE(Read(Write(desired), nativeReload, diagnostic) == Status::Ok);
	CHECK(nativeReload == desired);
	CHECK(Evaluate(nativeReload) == expected);
	std::vector<std::byte> bytes;
	saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(Evaluate(reopened.Graph) == expected);
	CHECK(reopened.Graph.Links == desired.Links);
	for (const auto &edit : edits) {
		const auto &control = std::get<PxcxInputValueEdit>(edit);
		CHECK(Authored(Native(reopened.Graph, "smooth"), control.Port) == control.Data);
	}
	const auto json = Source(reopened);
	CHECK(json["future_project"] == project["future_project"]);
	CHECK(json["nodes"][1] == project["nodes"][1]);
	CHECK(json["nodes"][2] == project["nodes"][2]);
	CHECK(json["nodes"][0]["future_node"] == "keep");
	for (const auto &input : json["nodes"][0]["inputs"])
		CHECK(input["future_input"] == "keep");
	CHECK(Native(reopened.Graph, "smooth").DynamicInputs[0].Default == std::optional<Value>{anchors[0]});
	CHECK(Native(reopened.Graph, "smooth").DynamicInputs[1].Default == std::optional<Value>{anchors[1]});
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, diagnostic));
	CHECK(unchanged == bytes);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE("Smooth sampling policy survives a durable native path value", "[pxcx_smooth_path]") {
	auto imported = Import(Project());
	SetStatic(imported.Graph, "smooth", "normalized_length", false);
	SetStatic(imported.Graph, "smooth", "sample_path", .75);
	SetStatic(imported.Graph, "sample", "ratio", .75);
	const auto evaluated = Evaluate(imported.Graph);
	CHECK(evaluated[1] == Value{Vector2{16, 0}});
	CHECK(evaluated[2] == Value{Vector2{16, 0}});
	CHECK(evaluated[3] == Value{31.0});
	const auto &path = std::get<Path2D>(evaluated[0]);
	REQUIRE(path.SourceSmooth);
	CHECK_FALSE(path.SourceSmooth->NormalizedLength);
	Document receipt;
	receipt.FormatVersion = 9;
	receipt.Nodes = {
		{"sample", "pc.path_sample", "", {}, {{"path", path}, {"ratio", .75}, {"type", EnumValue{2}}}},
		{"length", "pc.path_length", "", {}, {{"path", path}}}
	};
	receipt.Outputs = {{"sample", "sample", "position"}, {"length", "length", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(receipt), restored, diagnostic) == Status::Ok);
	CHECK(restored == receipt);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	for (size_t index = 0; index < restored.Outputs.size(); ++index) {
		EvaluatedValue output;
		REQUIRE(
			EvaluateValue(restored, plan, restored.Outputs[index].Id, {}, output, diagnostic) == Status::Ok
		);
		CHECK(output.Data == evaluated[index + 2]);
	}
}

TEST_CASE("PXC Smooth empty fixed records retain typed constructor controls", "[pxcx_smooth_path]") {
	auto project = Project();
	for (size_t index = 0; index < 6; ++index)
		project["nodes"][0]["inputs"][index] = Json::object();
	auto imported = Import(project);
	const auto &smooth = Native(imported.Graph, "smooth");
	REQUIRE(smooth.Type == "pc.path_smooth");
	const std::array<AuthoredValue, 6> controls{
		{{"loop", false},
		 {"round_anchor", false},
		 {"smoothness", 3.0},
		 {"sample_path", 0.0},
		 {"sample_mode", EnumValue{0}},
		 {"normalized_length", true}}
	};
	const auto *entry = FindCatalogueEntry(smooth.Type);
	REQUIRE(entry);
	for (const auto &control : controls) {
		const auto *input = FindCatalogueInput(*entry, control.Port);
		REQUIRE(input);
		CHECK(CatalogueDefault(*input) == std::optional<Value>{control.Data});
		CHECK(std::none_of(smooth.Values.begin(), smooth.Values.end(), [&](const auto &value) {
			return value.Port == control.Port;
		}));
	}
	const auto evaluated = Evaluate(imported.Graph);
	CHECK(evaluated[1] == Value{Vector2{0, 0}});
	CHECK(evaluated[2] == Value{Vector2{16, 0}});
	CHECK(evaluated[3] == Value{31.0});
}

TEST_CASE(
	"PXC Smooth dynamic input bypass keeps the anchor value through edit and reopen", "[pxcx_smooth_path]"
) {
	auto project = Project();
	project["nodes"][0]["inputs"][7]["bypass"] = true;
	auto consumer = project["nodes"][0];
	consumer["id"] = "consumer";
	consumer["inputs"][6] = Wire("smooth");
	consumer["inputs"][6]["from_index"] = 1007;
	consumer["inputs"][7] = Fixed(Json::array({0, 0}));
	project["nodes"].push_back(consumer);
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "smooth").Type == "pc.path_smooth");
	REQUIRE(Native(imported.Graph, "consumer").Type == "pc.path_smooth");
	auto desired = imported.Graph;
	auto &input = Native(desired, "smooth").DynamicInputs[1];
	input.Default = Vector2{48, 0};
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "smooth" && key.Port == input.Id) key.Data = Vector2{48, 0};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Source(reopened)["nodes"][3] == consumer);
	reopened.Graph.Outputs = {{"position", "consumer", "position_out"}};
	Plan plan;
	const auto compiled = Compile(reopened.Graph, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue output;
	const auto status = EvaluateValue(reopened.Graph, plan, "position", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Data == Value{Vector2{24, 0}});
}

TEST_CASE("PXC Smooth missing dynamic bypass remains opaque and preserves source", "[pxcx_smooth_path]") {
	auto project = Project();
	auto consumer = project["nodes"][0];
	consumer["id"] = "consumer";
	consumer["inputs"][6] = Wire("smooth");
	consumer["inputs"][6]["from_index"] = 1008;
	project["nodes"].push_back(std::move(consumer));
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "smooth").Type == "pxcx.opaque/Node_Path_Smooth");
	CHECK_FALSE(imported.Diagnostics.empty());
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
}
