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

TEST_SUITE_ID("engine.imagegraphio.pxcx_path_spiral")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}};
	}
	Json Wire(std::string node, int index = 0) {
		return {{"from_node", std::move(node)}, {"from_index", index}, {"from_tag", 0}};
	}
	Json Row(double time, double value, std::string identity) {
		return Json::array(
			{Json::array({0, time, "marker"}),
			 value,
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 16777215,
			 Json{{"future_key", std::move(identity)}}}
		);
	}
	Json CurveWords(double end) {
		return Json::array(
			{0,
			 1,
			 0,
			 0,
			 1,
			 0,
			 0,
			 0,
			 0,
			 0,
			 .3333333333333333,
			 end / 3,
			 -.3333333333333333,
			 -end / 3,
			 1,
			 end,
			 0,
			 0}
		);
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
		Json spiral = std::vector<Json>(15, Json::object());
		spiral[0] = Wire("shape");
		spiral[1] = {{"anim", true}, {"r", Json::array({Row(0, 2, "first"), Row(10, 4, "second")})}};
		spiral[2] = Fixed(1.25);
		spiral[2]["attri"] = {{"curved", true}};
		spiral[3] = Fixed(.6);
		spiral[4] = Fixed(15);
		spiral[5] = Fixed(CurveWords(1));
		spiral[6] = Fixed(Json::array({.1, .9}));
		spiral[7] = Fixed(true);
		spiral[8] = Fixed(true);
		spiral[9] = Fixed(true);
		spiral[10] = Fixed(1);
		spiral[11] = Fixed(Json::array({.5, 1.5}));
		spiral[12] = Fixed(1);
		spiral[13] = Fixed(Json::array({30, 120}));
		spiral[13]["attri"] = {{"curved", true}};
		spiral[14] = Fixed(CurveWords(1));
		for (auto &input : spiral)
			input["future_input"] = "keep";
		Json sample = std::vector<Json>(6, Json::object());
		sample[0] = Wire("spiral");
		sample[1] = Fixed(.37);
		sample[2] = Fixed(2);
		sample[3] = Fixed(Json::array({0, 1}));
		sample[4] = Fixed(0);
		sample[5] = Fixed(0);
		Json nodes = Json::array(
			{Record("shape", "Node_Path_Shape", shape),
			 Record("spiral", "Node_Path_Spiral", spiral),
			 Record("sample", "Node_Path_Sample", sample)}
		);
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
	PxcxImport Import(const Json &project) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		return imported;
	}
	Node &Native(Document &document, std::string_view id) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
			return value.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const AuthoredValue *Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return &value;
		return nullptr;
	}
	std::array<Value, 3> Evaluate(Document document) {
		document.Outputs = {
			{"path", "spiral", "path"}, {"position", "sample", "position"}, {"weight", "sample", "weight"}
		};
		Plan plan;
		Diagnostic error;
		auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.Tick = 5;
		std::array<Value, 3> values;
		const std::array<const char *, 3> outputs{"path", "position", "weight"};
		for (size_t i = 0; i < outputs.size(); ++i) {
			EvaluatedValue result;
			status = EvaluateValue(document, plan, outputs[i], request, result, error);
			INFO(error.Message);
			REQUIRE(status == Status::Ok);
			values[i] = std::move(result.Data);
		}
		CHECK(std::isfinite(std::get<Vector2>(values[1]).X));
		CHECK(std::isfinite(std::get<Vector2>(values[1]).Y));
		CHECK(std::isfinite(std::get<double>(values[2])));
		return values;
	}
	PxcxImport Reopen(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure));
		return result;
	}
	// Static source edits update the retained compact key as well as its authored value.
	Document StaticEdits(const PxcxImport &imported, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		auto result = Reopen(bytes).Graph;
		for (const auto &edit : edits) {
			const auto &operation = std::get<PxcxInputValueEdit>(edit);
			const auto *value = Authored(Native(result, operation.NodeId), operation.Port);
			REQUIRE(value);
			CHECK(value->Data == operation.Data);
			unsigned compactKeys = 0;
			for (const auto &key : result.Keyframes)
				if (key.NodeId == operation.NodeId && key.Port == operation.Port &&
					key.SourceKeyId == "pxc:compact") {
					CHECK(key.Data == operation.Data);
					++compactKeys;
				}
			CHECK(compactKeys == 1);
		}
		return result;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	void
	CheckEvaluation(const Document &desired, const Document &reopened, const std::array<Value, 3> &expected) {
		const auto actual = Evaluate(reopened);
		if (actual != expected) {
			INFO("Authored controls:\n" << Write(desired));
			INFO("Reopened controls:\n" << Write(reopened));
			Document receipts;
			receipts.FormatVersion = 9;
			receipts.Nodes = {
				{"expected", "pc.path_sample", "", {}, {{"path", expected[0]}}},
				{"actual", "pc.path_sample", "", {}, {{"path", actual[0]}}}
			};
			INFO("Evaluated path receipts:\n" << Write(receipts));
			const auto expectedPosition = std::get<Vector2>(expected[1]);
			const auto actualPosition = std::get<Vector2>(actual[1]);
			CAPTURE(expectedPosition.X, expectedPosition.Y, actualPosition.X, actualPosition.Y);
			CAPTURE(std::get<double>(expected[2]), std::get<double>(actual[2]));
			CHECK(actual[0] == expected[0]);
			CHECK(actual[1] == expected[1]);
			CHECK(actual[2] == expected[2]);
		} else {
			CHECK(actual == expected);
		}
	}
}

TEST_CASE(
	"PXC Spiral maps complete controls and preserves compound edits through reopen", "[pxcx_path_spiral]"
) {
	const auto project = Project();
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "spiral").Type == "pc.path_spiral");
	CHECK(
		imported.Graph.Links ==
		std::vector<Link>{{"shape", "path_data", "spiral", "path"}, {"spiral", "path", "sample", "path"}}
	);
	const auto *entry = FindCatalogueEntry("pc.path_spiral");
	REQUIRE(entry);
	REQUIRE(entry->Outputs.size() == 1);
	CHECK(entry->Outputs.front().Id == "path");
	const auto *angleCurved = Authored(Native(imported.Graph, "spiral"), "angle_curved");
	REQUIRE(angleCurved);
	CHECK(angleCurved->Data == Value{true});
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const auto before = Evaluate(imported.Graph);
	const auto *amplitudeCurve = Authored(Native(imported.Graph, "spiral"), "amplitude_curve");
	REQUIRE(amplitudeCurve);
	auto curve = std::get<Curve>(amplitudeCurve->Data);
	curve.Header[4] = .75;
	const std::array<PxcxEdit, 7> edits{
		PxcxInputValueEdit{"spiral", "phase", 45.0},
		PxcxInputValueEdit{"spiral", "amplitude", 2.0},
		PxcxInputValueEdit{"spiral", "weight_mode", EnumValue{2}},
		PxcxInputValueEdit{"spiral", "range_2", Vector2{.25, 2}},
		PxcxInputValueEdit{"spiral", "range", Vector2{0, 1}},
		PxcxInputValueEdit{"spiral", "loop", false},
		PxcxInputValueEdit{"spiral", "amplitude_curve", std::move(curve)}
	};
	Document desired = StaticEdits(imported, edits);
	unsigned moved = 0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "spiral" && key.Port == "frequency" && key.Tick == 10) {
			key.Tick = 12;
			key.Data = 6.0;
			++moved;
		}
	REQUIRE(moved == 1);
	const auto expected = Evaluate(desired);
	CHECK(expected[1] != before[1]);
	Document nativeReload;
	Diagnostic error;
	REQUIRE(Read(Write(desired), nativeReload, error) == Status::Ok);
	CHECK(nativeReload == desired);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CheckEvaluation(desired, reopened.Graph, expected);
	const auto json = Source(reopened);
	CHECK(json["future_project"] == project["future_project"]);
	CHECK(json["nodes"][0] == project["nodes"][0]);
	CHECK(json["nodes"][2] == project["nodes"][2]);
	const auto &record = json["nodes"][1];
	CHECK(record["future_node"] == "keep");
	REQUIRE(record["inputs"].size() == 15);
	for (const auto &input : record["inputs"])
		CHECK(input["future_input"] == "keep");
	const auto &keys = record["inputs"][1]["r"];
	REQUIRE(keys.size() == 2);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][1]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][1]["r"][1][9]);
	CHECK(keys[1][0][2] == "marker");
	CHECK(keys[1][0][1] == 12);
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
	CHECK(unchanged == bytes);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE("PXC Spiral grouping preserves path links and source membership", "[pxcx_path_spiral]") {
	auto imported = Import(Project(true));
	REQUIRE(Native(imported.Graph, "spiral").Type == "pc.path_spiral");
	CHECK(Native(imported.Graph, "spiral").GroupId == "group");
	const auto expected = Evaluate(imported.Graph);
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"spiral", "phase", -20.0}};
	auto desired = StaticEdits(imported, edits);
	std::vector<std::byte> bytes;
	Diagnostic error;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(Native(reopened.Graph, "spiral").GroupId == "group");
	CheckEvaluation(desired, reopened.Graph, Evaluate(desired));
	CHECK(Evaluate(reopened.Graph)[1] != expected[1]);
	CHECK(Source(reopened)["nodes"][3] == Source(imported)["nodes"][3]);
}

TEST_CASE(
	"PXC Spiral refuses incomplete control and output mappings without replacing source", "[pxcx_path_spiral]"
) {
	auto project = Project();
	SECTION("Malformed curve") {
		project["nodes"][1]["inputs"][5] = Fixed(Json::array({1, 2, 3}));
	}
	SECTION("Malformed frequency") {
		project["nodes"][1]["inputs"][1] = Fixed("unrepresented");
	}
	SECTION("Unknown Spiral output") {
		project["nodes"][2]["inputs"][0]["from_index"] = 1;
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "spiral").Type == "pxcx.opaque/Node_Path_Spiral");
	CHECK_FALSE(imported.Diagnostics.empty());
	const auto bytes = imported.Source.OriginalBytes;
	const auto graph = imported.Graph;
	std::vector<std::byte> saved;
	Diagnostic error;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, error));
	CHECK(saved == bytes);
	auto desired = imported.Graph;
	Native(desired, "spiral").Type = "pc.path_spiral";
	saved = {std::byte{0x7b}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, saved, error));
	CHECK(saved == std::vector<std::byte>{std::byte{0x7b}});
	CHECK(imported.Graph == graph);
	CHECK(imported.Source.OriginalBytes == bytes);
}
