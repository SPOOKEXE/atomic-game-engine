// Source-backed Repeat controls survive native and PXC saves, including grouped path routes.
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_path_repeat")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;

	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}};
	}
	Json Wire(std::string node) {
		return {{"from_node", std::move(node)}, {"from_index", 0}, {"from_tag", 0}};
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
	Json Project(bool grouped = false) {
		Json shape = std::vector<Json>(19, Json::object());
		shape[0] = Fixed(Json::array({0, 0}));
		shape[0]["unit"] = 0;
		shape[1] = Fixed(Json::array({5, 1}));
		shape[1]["unit"] = 0;
		shape[3] = Fixed("Line");
		// Indices come from Node_Path_Repeat at pinned source commit
		// b69eca232217360cf1502ef0223523d818606652.
		Json repeat = std::vector<Json>(13, Json::object());
		repeat[0] = Wire("shape");
		repeat[1] = Fixed(3);
		repeat[2] = Fixed(Json::array({2, -1}));
		repeat[3] = Fixed(15);
		repeat[4] = Fixed(Json::array({.5, 1.5}));
		repeat[5] = Fixed(Json::array({1, 2}));
		repeat[6] = Fixed(1);
		repeat[7] = Fixed(Json::array({3, 4}));
		repeat[8] = Fixed(Json::array({2, 3}));
		repeat[9] = Fixed(true);
		repeat[10] = Fixed(Json::array({-1, .5}));
		repeat[11] = Fixed(30);
		repeat[12] = Fixed(Json::array({2, .75}));
		for (size_t index : {2, 5, 7, 8, 10})
			repeat[index]["unit"] = 0;
		for (auto &input : repeat)
			input["future_input"] = "keep";
		Json nodes = Json::array(
			{Record("shape", "Node_Path_Shape", shape), Record("repeat", "Node_Path_Repeat", repeat)}
		);
		for (int index = 0; index < 4; ++index) {
			Json sample = std::vector<Json>(6, Json::object());
			sample[0] = Wire("repeat");
			sample[1] = Fixed(.37);
			sample[2] = Fixed(2);
			sample[3] = Fixed(Json::array({0, 1}));
			sample[4] = Fixed(0);
			sample[5] = Fixed(index);
			nodes.push_back(Record("sample" + std::to_string(index), "Node_Path_Sample", sample));
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
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure));
		return result;
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
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
			return value.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const AuthoredValue &Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return value;
		FAIL("missing mapped Repeat control: " << port);
		return node.Values.front();
	}
	std::vector<Value> Evaluate(Document document, size_t copies) {
		document.Outputs = {{"path", "repeat", "path"}};
		for (size_t index = 0; index < copies; ++index) {
			const auto id = "sample" + std::to_string(index);
			document.Outputs.push_back({id + "/position", id, "position"});
			document.Outputs.push_back({id + "/weight", id, "weight"});
		}
		Plan plan;
		Diagnostic error;
		const auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		std::vector<Value> values;
		for (const auto &output : document.Outputs) {
			EvaluatedValue result;
			const auto evaluated = EvaluateValue(document, plan, output.Id, {}, result, error);
			INFO(error.Message);
			REQUIRE(evaluated == Status::Ok);
			values.push_back(std::move(result.Data));
		}
		const auto &path = std::get<Path2D>(values.front());
		REQUIRE(path.SourceOperation);
		CHECK(path.SourceOperation->Kind == SourcePathOperationKind::Repeat);
		CHECK(path.SourceOperation->Inputs.size() == copies);
		for (size_t index = 1; index < values.size(); index += 2) {
			CHECK(std::isfinite(std::get<Vector2>(values[index]).X));
			CHECK(std::isfinite(std::get<Vector2>(values[index]).Y));
			CHECK(std::get<double>(values[index + 1]) == 1);
		}
		return values;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	Document StaticEdits(const PxcxImport &imported, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		auto desired = Reopen(bytes).Graph;
		for (const auto &edit : edits) {
			const auto &operation = std::get<PxcxInputValueEdit>(edit);
			CHECK(Authored(Native(desired, operation.NodeId), operation.Port).Data == operation.Data);
		}
		return desired;
	}
}

TEST_CASE(
	"PXC Repeat maps all source controls and retains edited path sampling through reopen",
	"[pxcx_path_repeat]"
) {
	const auto project = Project();
	auto imported = Import(project);
	const auto &repeat = Native(imported.Graph, "repeat");
	REQUIRE(repeat.Type == "pc.path_repeat");
	const std::array<AuthoredValue, 12> controls{
		{{"amount", int64_t{3}},
		 {"shift_position", Vector2{2, -1}},
		 {"shift_rotation", 15.0},
		 {"shift_scale", Vector2{.5, 1.5}},
		 {"anchor", Vector2{1, 2}},
		 {"pattern", EnumValue{1}},
		 {"center", Vector2{3, 4}},
		 {"radius", Vector2{2, 3}},
		 {"rotate_along", true},
		 {"position", Vector2{-1, .5}},
		 {"rotation", 30.0},
		 {"scale", Vector2{2, .75}}}
	};
	for (const auto &control : controls)
		CHECK(Authored(repeat, control.Port).Data == control.Data);
	REQUIRE(imported.Graph.Links.size() == 5);
	CHECK(imported.Graph.Links.front() == Link{"shape", "path_data", "repeat", "path"});
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const auto before = Evaluate(imported.Graph, 3);
	const std::array<PxcxEdit, 12> edits{
		PxcxInputValueEdit{"repeat", "amount", int64_t{4}},
		PxcxInputValueEdit{"repeat", "pattern", EnumValue{0}},
		PxcxInputValueEdit{"repeat", "shift_position", Vector2{-3, 2}},
		PxcxInputValueEdit{"repeat", "shift_rotation", -25.0},
		PxcxInputValueEdit{"repeat", "shift_scale", Vector2{1.25, .5}},
		PxcxInputValueEdit{"repeat", "anchor", Vector2{-.5, 1}},
		PxcxInputValueEdit{"repeat", "center", Vector2{5, -4}},
		PxcxInputValueEdit{"repeat", "radius", Vector2{4, 2}},
		PxcxInputValueEdit{"repeat", "rotate_along", false},
		PxcxInputValueEdit{"repeat", "position", Vector2{2, -1}},
		PxcxInputValueEdit{"repeat", "rotation", 70.0},
		PxcxInputValueEdit{"repeat", "scale", Vector2{.75, 2}}
	};
	auto desired = StaticEdits(imported, edits);
	const auto expected = Evaluate(desired, 4);
	CHECK(expected[1] != before[1]);
	Document nativeReload;
	Diagnostic error;
	REQUIRE(Read(Write(desired), nativeReload, error) == Status::Ok);
	CHECK(nativeReload == desired);
	CHECK(Evaluate(nativeReload, 4) == expected);
	Document receipt;
	receipt.FormatVersion = 9;
	receipt.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", expected[0]}}}};
	Document receiptReload;
	REQUIRE(Read(Write(receipt), receiptReload, error) == Status::Ok);
	CHECK(receiptReload == receipt);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(Evaluate(reopened.Graph, 4) == expected);
	CHECK(reopened.Graph.Links == desired.Links);
	const auto json = Source(reopened);
	CHECK(json["future_project"] == project["future_project"]);
	CHECK(json["nodes"][0] == project["nodes"][0]);
	for (size_t index = 2; index < 6; ++index)
		CHECK(json["nodes"][index] == project["nodes"][index]);
	CHECK(json["nodes"][1]["future_node"] == "keep");
	REQUIRE(json["nodes"][1]["inputs"].size() == 13);
	for (const auto &input : json["nodes"][1]["inputs"])
		CHECK(input["future_input"] == "keep");
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
	CHECK(unchanged == bytes);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE("PXC Repeat grouped and ungrouped routes retain identical samples", "[pxcx_path_repeat]") {
	auto grouped = Import(Project(true));
	auto ungrouped = Import(Project());
	REQUIRE(Native(grouped.Graph, "repeat").Type == "pc.path_repeat");
	CHECK(Native(grouped.Graph, "repeat").GroupId == "group");
	CHECK(Evaluate(grouped.Graph, 3) == Evaluate(ungrouped.Graph, 3));
	const std::array<PxcxEdit, 2> edits{
		PxcxInputValueEdit{"repeat", "rotation", -40.0},
		PxcxInputValueEdit{"repeat", "shift_scale", Vector2{1.5, .25}}
	};
	auto desired = StaticEdits(grouped, edits);
	const auto expected = Evaluate(desired, 3);
	CHECK(expected[1] != Evaluate(grouped.Graph, 3)[1]);
	CHECK(expected == Evaluate(StaticEdits(ungrouped, edits), 3));
	std::vector<std::byte> bytes;
	Diagnostic error;
	const bool saved = WritePxcxProjection(grouped, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(Native(reopened.Graph, "shape").GroupId == "group");
	CHECK(Native(reopened.Graph, "repeat").GroupId == "group");
	for (size_t index = 0; index < 4; ++index)
		CHECK(Native(reopened.Graph, "sample" + std::to_string(index)).GroupId == "group");
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Evaluate(reopened.Graph, 3) == expected);
	CHECK(Source(reopened)["nodes"][6] == Source(grouped)["nodes"][6]);
}

TEST_CASE(
	"PXC Repeat retains all five physical units with non-square project dimensions", "[pxcx_path_repeat]"
) {
	const std::array<std::pair<size_t, const char *>, 5> physicalControls{
		{{2, "shift_position"}, {5, "anchor"}, {7, "center"}, {8, "radius"}, {10, "position"}}
	};
	std::array<std::vector<Value>, 2> samples;
	for (int64_t unit = 0; unit <= 1; ++unit) {
		CAPTURE(unit);
		auto project = Project();
		project["attributes"] = {{"surface_dimension", Json::array({32, 12})}};
		for (const auto &[index, port] : physicalControls)
			project["nodes"][1]["inputs"][index]["unit"] = unit;
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "repeat").Type == "pc.path_repeat");
		for (const auto &[index, port] : physicalControls)
			CHECK(
				Authored(Native(imported.Graph, "repeat"), std::string(port) + "_unit").Data ==
				Value{EnumValue{unit}}
			);
		const std::array<PxcxEdit, 5> edits{
			PxcxInputValueEdit{"repeat", "shift_position", Vector2{-3, 2}},
			PxcxInputValueEdit{"repeat", "anchor", Vector2{-.5, 1}},
			PxcxInputValueEdit{"repeat", "center", Vector2{5, -4}},
			PxcxInputValueEdit{"repeat", "radius", Vector2{4, 2}},
			PxcxInputValueEdit{"repeat", "position", Vector2{2, -1}}
		};
		auto desired = StaticEdits(imported, edits);
		samples[unit] = Evaluate(desired, 3);
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		auto reopened = Reopen(bytes);
		CHECK(Evaluate(reopened.Graph, 3) == samples[unit]);
		const auto source = Source(reopened);
		CHECK(source["attributes"] == project["attributes"]);
		for (const auto &[index, port] : physicalControls) {
			CHECK(source["nodes"][1]["inputs"][index]["unit"] == unit);
			CHECK(
				Authored(Native(reopened.Graph, "repeat"), std::string(port) + "_unit").Data ==
				Value{EnumValue{unit}}
			);
		}
	}
	CHECK(samples[0][1] != samples[1][1]);
}

TEST_CASE(
	"Malformed native Repeat wrappers refuse replacement of the previous document", "[pxcx_path_repeat]"
) {
	const auto imported = Import(Project());
	const auto evaluated = Evaluate(imported.Graph, 3);
	auto malformedPath = std::get<Path2D>(evaluated.front());
	REQUIRE(malformedPath.SourceOperation);
	REQUIRE_FALSE(malformedPath.SourceOperation->Inputs.empty());
	auto &copy = malformedPath.SourceOperation->Inputs.front();
	REQUIRE(copy.SourceOperation);
	SECTION("Missing owned transform wrapper") {
		copy = Path2D{};
	}
	SECTION("Outer wrapper rotates as well as scales") {
		copy.SourceOperation->TransformRotation = 15;
	}
	SECTION("Rotation wrapper also scales") {
		REQUIRE(copy.SourceOperation->Inputs.front().SourceOperation);
		copy.SourceOperation->Inputs.front().SourceOperation->TransformScale = {2, 1};
	}
	SECTION("Transform wrappers disagree about anchor") {
		REQUIRE(copy.SourceOperation->Inputs.front().SourceOperation);
		copy.SourceOperation->Inputs.front().SourceOperation->TransformAnchor.X += 1;
	}
	Document malformed;
	malformed.FormatVersion = 9;
	malformed.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", std::move(malformedPath)}}}};
	auto retained = imported.Graph;
	const auto previous = retained;
	Diagnostic error;
	CHECK(Read(Write(malformed), retained, error) != Status::Ok);
	CHECK(retained == previous);
}
