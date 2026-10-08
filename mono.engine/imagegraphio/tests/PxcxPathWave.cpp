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
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_path_wave")

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
	Json Row(double time, Json value, std::string identity) {
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
		Json wave = std::vector<Json>(25, Json::object());
		wave[0] = Wire("shape");
		wave[1] = Fixed(Json::array({2, 3}));
		wave[2] = Fixed(Json::array({1, 2}));
		wave[2]["attri"] = {{"curved", true}};
		wave[3] = {
			{"anim", true},
			{"r",
			 Json::array({Row(0, Json::array({0, 0}), "first"), Row(10, Json::array({.5, .8}), "second")})}
		};
		wave[4] = Fixed(1);
		wave[5] = Fixed(17);
		wave[6] = Fixed(true);
		wave[7] = Fixed(Json::array({-.3, .4}));
		wave[8] = Fixed(3);
		wave[9] = Fixed(CurveWords(1));
		wave[10] = Fixed(0);
		wave[11] = Fixed(Json::array({.1, .9}));
		wave[12] = Fixed(true);
		wave[13] = Fixed(2);
		wave[14] = Fixed(1.25);
		wave[15] = Fixed(.6);
		wave[16] = Fixed(.15);
		wave[17] = Fixed(.1);
		wave[18] = Fixed(true);
		wave[19] = Fixed(true);
		wave[20] = Fixed(1);
		wave[21] = Fixed(Json::array({.5, 1.5}));
		wave[22] = Fixed(1);
		wave[23] = Fixed(Json::array({30, 80}));
		wave[23]["attri"] = {{"curved", true}};
		wave[24] = Fixed(CurveWords(1));
		for (auto &input : wave)
			input["future_input"] = "keep";
		Json sample = std::vector<Json>(6, Json::object());
		sample[0] = Wire("wave");
		sample[1] = Fixed(.37);
		sample[2] = Fixed(2);
		sample[3] = Fixed(Json::array({0, 1}));
		sample[4] = Fixed(0);
		sample[5] = Fixed(0);
		Json nodes = Json::array(
			{Record("shape", "Node_Path_Shape", shape),
			 Record("wave", "Node_Path_Wave", wave),
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
	std::array<Value, 3> Evaluate(Document document, int64_t tick = 5) {
		document.Outputs = {
			{"path", "wave", "path"}, {"position", "sample", "position"}, {"weight", "sample", "weight"}
		};
		Plan plan;
		Diagnostic error;
		auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		std::array<Value, 3> values;
		const std::array<const char *, 3> outputs{"path", "position", "weight"};
		for (size_t i = 0; i < outputs.size(); ++i) {
			EvaluatedValue result;
			status = EvaluateValue(document, plan, outputs[i], request, result, error);
			INFO(error.Message);
			REQUIRE(status == Status::Ok);
			values[i] = std::move(result.Data);
		}
		if (const auto *position = std::get_if<Vector2>(&values[1])) {
			CHECK(std::isfinite(position->X));
			CHECK(std::isfinite(position->Y));
			CHECK(std::isfinite(std::get<double>(values[2])));
		} else {
			const auto &positions = std::get<ArrayValue>(values[1]);
			const auto &weights = std::get<ArrayValue>(values[2]);
			CHECK(positions.ElementType == ValueType::Vector2);
			CHECK(weights.ElementType == ValueType::Scalar);
			CHECK(positions.Elements.size() == weights.Elements.size());
		}
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
	void CheckEvaluation(const Document &, const Document &reopened, const std::array<Value, 3> &expected) {
		CHECK(Evaluate(reopened) == expected);
	}

}

TEST_CASE("PXC Wave maps complete controls and preserves compound edits through reopen", "[pxcx_path_wave]") {
	const auto project = Project();
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "wave").Type == "pc.path_wave");
	CHECK(
		imported.Graph.Links ==
		std::vector<Link>{{"shape", "path_data", "wave", "path"}, {"wave", "path", "sample", "path"}}
	);
	const auto *entry = FindCatalogueEntry("pc.path_wave");
	REQUIRE(entry);
	REQUIRE(entry->Outputs.size() == 1);
	CHECK(entry->Outputs.front().Id == "path");
	unsigned physicalControls = 0;
	for (const auto &control : entry->Inputs) {
		if (control.SourceIndex < 0) continue;
		++physicalControls;
		if (control.Id != "path" && control.Id != "phase")
			CHECK(Authored(Native(imported.Graph, "wave"), control.Id));
	}
	CHECK(physicalControls == 25);
	const auto *angleCurved = Authored(Native(imported.Graph, "wave"), "angle_curved");
	REQUIRE(angleCurved);
	CHECK(angleCurved->Data == Value{true});
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	for (const char *port : {"amplitude_curved", "angle_curved"}) {
		CAPTURE(port);
		const std::array<PxcxEdit, 1> unsupported{PxcxInputValueEdit{"wave", port, false}};
		const std::vector<std::byte> sentinel{std::byte{0x61}};
		auto refused = sentinel;
		Diagnostic refusal;
		CHECK_FALSE(WritePxcxEdits(imported, originalBytes, unsupported, refused, refusal));
		CHECK(refused == sentinel);
		CHECK(imported.Graph == originalGraph);
		CHECK(imported.Source.OriginalBytes == originalBytes);
	}
	const auto before = Evaluate(imported.Graph);
	const auto *amplitudeCurve = Authored(Native(imported.Graph, "wave"), "amplitude_curve");
	REQUIRE(amplitudeCurve);
	auto curve = std::get<Curve>(amplitudeCurve->Data);
	curve.Header[4] = .75;
	const std::array<PxcxEdit, 23> edits{
		PxcxInputValueEdit{"wave", "frequency", Vector2{1.5, 2.5}},
		PxcxInputValueEdit{"wave", "amplitude", Vector2{2, 3}},
		PxcxInputValueEdit{"wave", "mode", EnumValue{2}},
		PxcxInputValueEdit{"wave", "seed", 23.0},
		PxcxInputValueEdit{"wave", "wiggle", false},
		PxcxInputValueEdit{"wave", "wiggle_amplitude", Vector2{-.5, .8}},
		PxcxInputValueEdit{"wave", "wiggle_frequency", 5.0},
		PxcxInputValueEdit{"wave", "amplitude_curve", std::move(curve)},
		PxcxInputValueEdit{"wave", "post_fn", EnumValue{1}},
		PxcxInputValueEdit{"wave", "range", Vector2{0, 1}},
		PxcxInputValueEdit{"wave", "clamp_curve", false},
		PxcxInputValueEdit{"wave", "iteration", int64_t{3}},
		PxcxInputValueEdit{"wave", "freqency", 2.25},
		PxcxInputValueEdit{"wave", "amplitude_2", .4},
		PxcxInputValueEdit{"wave", "shift_2", .3},
		PxcxInputValueEdit{"wave", "shift", .2},
		PxcxInputValueEdit{"wave", "loop", false},
		PxcxInputValueEdit{"wave", "use_weight", false},
		PxcxInputValueEdit{"wave", "weight_mode", EnumValue{2}},
		PxcxInputValueEdit{"wave", "range_2", Vector2{.25, 2}},
		PxcxInputValueEdit{"wave", "direction", EnumValue{0}},
		PxcxInputValueEdit{"wave", "angle", Vector2{45, 120}},
		PxcxInputValueEdit{
			"wave",
			"angle_curve",
			std::get<Curve>(Authored(Native(imported.Graph, "wave"), "angle_curve")->Data)
		}
	};
	Document desired = StaticEdits(imported, edits);
	unsigned moved = 0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "wave" && key.Port == "phase" && key.Tick == 10) {
			key.Tick = 12;
			key.Data = Vector2{1.2, 1.5};
			++moved;
		}
	REQUIRE(moved == 1);
	const auto expected = Evaluate(desired);
	CHECK(expected[1] != before[1]);
	Document nativeReload;
	Diagnostic error;
	REQUIRE(Read(Write(desired), nativeReload, error) == Status::Ok);
	CHECK(nativeReload == desired);
	CHECK(Evaluate(nativeReload) == expected);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CheckEvaluation(desired, reopened.Graph, expected);
	for (int64_t tick : {0, 5, 12}) {
		CAPTURE(tick);
		CHECK(Evaluate(reopened.Graph, tick) == Evaluate(desired, tick));
		CHECK(Evaluate(reopened.Graph, tick) == Evaluate(nativeReload, tick));
	}
	const auto json = Source(reopened);
	CHECK(json["future_project"] == project["future_project"]);
	CHECK(json["nodes"][0] == project["nodes"][0]);
	CHECK(json["nodes"][2] == project["nodes"][2]);
	const auto &record = json["nodes"][1];
	CHECK(record["future_node"] == "keep");
	REQUIRE(record["inputs"].size() == 25);
	CHECK(record["inputs"][2]["attri"] == project["nodes"][1]["inputs"][2]["attri"]);
	CHECK(record["inputs"][23]["attri"] == project["nodes"][1]["inputs"][23]["attri"]);
	for (const auto &input : record["inputs"])
		CHECK(input["future_input"] == "keep");
	const auto &keys = record["inputs"][3]["r"];
	REQUIRE(keys.size() == 2);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][3]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][3]["r"][1][9]);
	CHECK(keys[1][0][2] == "marker");
	CHECK(keys[1][0][1] == 12);
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
	CHECK(unchanged == bytes);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE("PXC Wave grouping preserves path links and source membership", "[pxcx_path_wave]") {
	auto imported = Import(Project(true));
	REQUIRE(Native(imported.Graph, "wave").Type == "pc.path_wave");
	CHECK(Native(imported.Graph, "wave").GroupId == "group");
	const auto expected = Evaluate(imported.Graph);
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"wave", "shift", .6}};
	auto desired = StaticEdits(imported, edits);
	std::vector<std::byte> bytes;
	Diagnostic error;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(Native(reopened.Graph, "wave").GroupId == "group");
	CheckEvaluation(desired, reopened.Graph, Evaluate(desired));
	CHECK(Evaluate(reopened.Graph)[1] != expected[1]);
	CHECK(Source(reopened)["nodes"][3] == Source(imported)["nodes"][3]);
}

TEST_CASE(
	"PXC Wave refuses incomplete control and output mappings without replacing source", "[pxcx_path_wave]"
) {
	auto project = Project();
	SECTION("Malformed curve") {
		project["nodes"][1]["inputs"][9] = Fixed(Json::array({1, 2, 3}));
	}
	SECTION("Malformed frequency") {
		project["nodes"][1]["inputs"][1] = Fixed("unrepresented");
	}
	SECTION("Unknown Wave output") {
		project["nodes"][2]["inputs"][0]["from_index"] = 1;
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "wave").Type == "pxcx.opaque/Node_Path_Wave");
	CHECK_FALSE(imported.Diagnostics.empty());
	const auto bytes = imported.Source.OriginalBytes;
	const auto graph = imported.Graph;
	std::vector<std::byte> saved;
	Diagnostic error;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, error));
	CHECK(saved == bytes);
	auto desired = imported.Graph;
	Native(desired, "wave").Type = "pc.path_wave";
	saved = {std::byte{0x7b}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, saved, error));
	CHECK(saved == std::vector<std::byte>{std::byte{0x7b}});
	CHECK(imported.Graph == graph);
	CHECK(imported.Source.OriginalBytes == bytes);
}

TEST_CASE("PXC Wave linked and array ranges retain grouping and seeded replay", "[pxcx_path_wave]") {
	auto project = Project(true);
	SECTION("Linked frequency range") {
		Json control = std::vector<Json>(12, Json::object());
		control[0] = Fixed(1.5);
		control[1] = Fixed(2.5);
		auto provider = Record("frequency", "Node_Vector2", control);
		provider["group"] = "group";
		project["nodes"].push_back(std::move(provider));
		project["nodes"][1]["inputs"][1] = Wire("frequency");
	}
	SECTION("Two authored processor ranges") {
		auto &inputs = project["nodes"][1]["inputs"];
		inputs[1] = Fixed(Json::array({Json::array({1, 1}), Json::array({3, 3})}));
		inputs[2] = Fixed(Json::array({1, 1}));
		inputs[3] = Fixed(Json::array({0, 0}));
		inputs[4] = Fixed(1);
		inputs[6] = Fixed(false);
		inputs[13] = Fixed(1);
		inputs[23] = Fixed(Json::array({90, 90}));
	}
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "wave").Type == "pc.path_wave");
	CHECK(Native(imported.Graph, "wave").GroupId == "group");
	const std::array<PxcxEdit, 2> edits{
		PxcxInputValueEdit{"wave", "seed", 37.0}, PxcxInputValueEdit{"wave", "shift_2", .35}
	};
	auto desired = StaticEdits(imported, edits);
	const auto expected = Evaluate(desired);
	CHECK(Evaluate(desired) == expected);
	Document nativeReload;
	Diagnostic error;
	REQUIRE(Read(Write(desired), nativeReload, error) == Status::Ok);
	CHECK(Evaluate(nativeReload) == expected);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reopened = Reopen(bytes);
	CHECK(reopened.Graph.Links == desired.Links);
	CHECK(Native(reopened.Graph, "wave").GroupId == "group");
	CHECK(Evaluate(reopened.Graph) == expected);
	CHECK(Evaluate(reopened.Graph) == expected);
	if (const auto *paths = std::get_if<ArrayValue>(&expected[0])) {
		CHECK(paths->ElementType == ValueType::Path2D);
		REQUIRE(paths->Elements.size() == 2);
		for (size_t index = 0; index < paths->Elements.size(); ++index) {
			const auto &path = std::get<Path2D>(paths->Elements[index]);
			REQUIRE(path.SourceOperation);
			REQUIRE(path.SourceOperation->Wave);
			CHECK(path.SourceOperation->Wave->Seed == 37 + double(index));
			CHECK(
				path.SourceOperation->Wave->Frequency == Vector2{1 + 2 * double(index), 1 + 2 * double(index)}
			);
		}
		const auto &positions = std::get<ArrayValue>(expected[1]);
		REQUIRE(positions.Elements.size() == 2);
		CHECK(positions.ElementType == ValueType::Vector2);
	} else {
		CHECK(std::any_of(reopened.Graph.Links.begin(), reopened.Graph.Links.end(), [](const auto &link) {
			return link == Link{"frequency", "vector", "wave", "frequency"};
		}));
	}
	CHECK(Source(reopened)["nodes"][1]["inputs"][1] == project["nodes"][1]["inputs"][1]);
}

TEST_CASE("Native Wave payloads roundtrip and malformed descriptors refuse replacement", "[pxcx_path_wave]") {
	auto imported = Import(Project());
	const auto evaluated = Evaluate(imported.Graph);
	auto path = std::get<Path2D>(evaluated[0]);
	REQUIRE(path.SourceOperation);
	CHECK(path.SourceOperation->Kind == SourcePathOperationKind::Wave);
	REQUIRE(path.SourceOperation->Wave);
	CHECK(path.SourceOperation->Wave->Seed == 17);
	Document receipt;
	receipt.FormatVersion = 9;
	receipt.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", path}}}};
	Diagnostic error;
	Document restored;
	REQUIRE(Read(Write(receipt), restored, error) == Status::Ok);
	CHECK(restored == receipt);
	bool missingDescriptor = false;
	SECTION("Missing Wave payload") {
		missingDescriptor = true;
	}
	SECTION("Invalid mode") {
		path.SourceOperation->Wave->Mode = 255;
	}
	SECTION("Invalid post function") {
		path.SourceOperation->Wave->Post = 255;
	}
	SECTION("Negative iteration") {
		path.SourceOperation->Wave->Iteration = -1;
	}
	SECTION("Truncated amplitude curve table") {
		REQUIRE_FALSE(path.SourceOperation->Wave->AmplitudeCurve.empty());
		path.SourceOperation->Wave->AmplitudeCurve.pop_back();
	}
	SECTION("Invalid iteration bound") {
		path.SourceOperation->Wave->Iteration = 4097;
	}
	SECTION("More than one planar source child") {
		REQUIRE(path.SourceOperation->Inputs.size() == 1);
		path.SourceOperation->Inputs.push_back(path.SourceOperation->Inputs.front());
	}
	receipt.Nodes[0].Values[0].Data = std::move(path);
	const auto previous = restored;
	auto serialized = Write(receipt);
	if (missingDescriptor) {
		const auto marker = serialized.find("wave ");
		REQUIRE(marker != std::string::npos);
		serialized.resize(marker + std::string("wave ").size());
	}
	CHECK(Read(serialized, restored, error) != Status::Ok);
	CHECK(restored == previous);
}
