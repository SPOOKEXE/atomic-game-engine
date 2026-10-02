#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcxstructureedit")

using namespace engine::imagegraphio;
using namespace engine::imagegraph;

namespace {
	using Json = nlohmann::ordered_json;
	PxcxImport Imported(
		std::string json =
			R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":3},"future":{"keep":1}}],"future":{"node":7}},{"id":"text","type":"Node_String_Merge","x":0,"y":0,"inputs":[{"r":{"d":"a"},"future":{"key":8}},{"r":{"d":"b"}}]},{"id":"opaque","type":"vendor.future","x":0,"y":0,"inputs":[{"r":{"d":9},"from_node":"number","from_index":0,"from_tag":12,"future":{"input":1}}]}],"future":{"project":[1,2,3]}})"
	) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = std::move(json);
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		const bool written = engine::bake::WritePxcx(archive, bytes, failure);
		INFO(failure);
		REQUIRE(written);
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		const bool imported = ImportPxcxImageGraph(archive, result, failure);
		INFO(failure);
		REQUIRE(imported);
		return result;
	}
	PxcxImport Edited(const PxcxImport &source, std::span<const PxcxStructureEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool accepted =
			WritePxcxStructureEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure));
		CHECK(result.Source.MetadataPayload == source.Source.MetadataPayload);
		CHECK(result.Source.ThumbnailRgba == source.Source.ThumbnailRgba);
		return result;
	}
	Json Graph(const PxcxImport &source) {
		return Json::parse(source.Source.GraphJson.begin(), source.Source.GraphJson.end() - 1);
	}
}

TEST_CASE(
	"PXC source node creation clone links and deletion are one native validated transaction",
	"[imagegraphio][pxcx_structure]"
) {
	const auto original = Imported();
	const std::array<PxcxStructureEdit, 3> edits{
		PxcxNodeInsert{
			R"({"id":"added","type":"Node_Number_Simple","x":4,"y":5,"inputs":[{"r":{"d":17}}],"future":{"new":1}})"
		},
		PxcxNodeClone{"number", "copy", {7, 8}},
		PxcxLinkEdit{"opaque", 0, PxcxSourceConnection{"added", 0, 14}}
	};
	const auto result = Edited(original, edits);
	REQUIRE(result.Source.Nodes.size() == 5);
	const auto json = Graph(result);
	CHECK(json["future"] == Graph(original)["future"]);
	CHECK(json["nodes"][0] == Graph(original)["nodes"][0]);
	CHECK(json["nodes"][4]["future"]["node"] == 7);
	CHECK(json["nodes"][4]["x"] == 7);
	CHECK(json["nodes"][2]["inputs"][0]["r"]["d"] == 9);
	CHECK(json["nodes"][2]["inputs"][0]["from_node"] == "added");
	CHECK(json["nodes"][2]["inputs"][0]["from_tag"] == 14);
	const std::array<PxcxStructureEdit, 1> remove{PxcxNodeDelete{"added"}};
	const auto deleted = Edited(result, remove);
	CHECK(deleted.Source.Nodes.size() == 4);
	CHECK(deleted.Source.Links.empty());
	const auto deletedJson = Graph(deleted);
	const auto &input = deletedJson["nodes"][2]["inputs"][0];
	CHECK_FALSE(input.contains("from_node"));
	CHECK_FALSE(input.contains("from_index"));
	CHECK_FALSE(input.contains("from_tag"));
	CHECK(input["r"]["d"] == 9);
	CHECK(input["future"]["input"] == 1);
}

TEST_CASE(
	"PXC dynamic insertion and deletion preserve authored order inactive values and metadata",
	"[imagegraphio][pxcx_structure]"
) {
	const auto original = Imported();
	const std::array<PxcxStructureEdit, 1> insert{
		PxcxDynamicInputInsert{"text", 1, R"([{"r":{"d":"middle"},"future":{"insert":2}}])"}
	};
	const auto inserted = Edited(original, insert);
	const auto json = Graph(inserted);
	REQUIRE(json["nodes"][1]["inputs"].size() == 3);
	CHECK(json["nodes"][1]["inputs"][0] == Graph(original)["nodes"][1]["inputs"][0]);
	CHECK(json["nodes"][1]["inputs"][1]["r"]["d"] == "middle");
	CHECK(json["nodes"][1]["inputs"][2]["r"]["d"] == "b");
	const std::array<PxcxStructureEdit, 1> remove{PxcxDynamicInputDelete{"text", 1, 1}};
	const auto removed = Edited(inserted, remove);
	CHECK(Graph(removed) == Graph(original));
	const std::array<PxcxStructureEdit, 2> noChange{insert[0], remove[0]};
	const auto identical = Edited(original, noChange);
	CHECK(identical.Source.OriginalBytes == original.Source.OriginalBytes);
}

TEST_CASE(
	"PXC identical structural transactions preserve exact archive bytes", "[imagegraphio][pxcx_structure]"
) {
	const auto source = Imported();
	const std::array<PxcxStructureEdit, 1> same{
		PxcxLinkEdit{"opaque", 0, PxcxSourceConnection{"number", 0, 12}}
	};
	CHECK(Edited(source, same).Source.OriginalBytes == source.Source.OriginalBytes);
	CHECK(Edited(source, {}).Source.OriginalBytes == source.Source.OriginalBytes);
}

TEST_CASE(
	"PXC malformed stale and dangling structural edits fail atomically", "[imagegraphio][pxcx_structure]"
) {
	const auto source = Imported();
	const std::array<PxcxStructureEdit, 10> invalid{
		PxcxNodeInsert{"{"},
		PxcxNodeInsert{
			R"({"id":"bad","id":"duplicate","type":"Node_Number_Simple","x":0,"y":0,"inputs":[]})"
		},
		PxcxNodeInsert{R"({"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[]})"},
		PxcxNodeInsert{
			R"({"id":"bad","type":"future","x":0,"y":0,"inputs":[{"from_node":"missing","from_index":0}]})"
		},
		PxcxNodeClone{"missing", "new", {}},
		PxcxLinkEdit{"opaque", 0, PxcxSourceConnection{"number", 888, {}}},
		PxcxLinkEdit{"opaque", 77, {}},
		PxcxDynamicInputInsert{"number", 0, R"([{"r":{"d":1}}])"},
		PxcxDynamicInputInsert{"text", 9, R"([{"r":{"d":1}}])"},
		PxcxDynamicInputDelete{"text", 1, 2}
	};
	for (const auto &edit : invalid) {
		std::vector<std::byte> bytes{std::byte{42}};
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxStructureEdits(
			source, source.Source.OriginalBytes, std::span(&edit, 1), bytes, diagnostic
		));
		CHECK(bytes == std::vector<std::byte>{std::byte{42}});
		CHECK_FALSE(diagnostic.Message.empty());
	}
	std::vector<std::byte> bytes{std::byte{42}};
	Diagnostic diagnostic;
	auto changed = source;
	changed.Source.GraphJson[0] = '[';
	CHECK_FALSE(WritePxcxStructureEdits(changed, source.Source.OriginalBytes, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
	CHECK_FALSE(WritePxcxStructureEdits(source, {}, {}, bytes, diagnostic));
}

TEST_CASE(
	"PXC structural JSON is bounded before parse and dynamic input groups keep their width",
	"[imagegraphio][pxcx_structure]"
) {
	const auto source = Imported();
	std::string deep(1000, '[');
	deep += '0';
	deep.append(1000, ']');
	const std::array<PxcxStructureEdit, 3> invalid{
		PxcxNodeInsert{std::string(Limits::MaximumArrayBytes + 1, ' ')},
		PxcxNodeInsert{deep},
		PxcxDynamicInputInsert{"text", 0, "[0]"}
	};
	for (const auto &edit : invalid) {
		std::vector<std::byte> bytes{std::byte{31}};
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxStructureEdits(
			source, source.Source.OriginalBytes, std::span(&edit, 1), bytes, diagnostic
		));
		CHECK(bytes == std::vector<std::byte>{std::byte{31}});
	}
	const auto pairs = Imported(
		R"({"nodes":[{"id":"format","type":"Node_String_Format","x":0,"y":0,"inputs":[{"r":{"d":"{key}"}},{"r":{"d":"key"}},{"r":{"d":"first"}}]}]})"
	);
	const std::array<PxcxStructureEdit, 1> incomplete{
		PxcxDynamicInputInsert{"format", 1, R"([{"r":{"d":"unpaired"}}])"}
	};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxStructureEdits(pairs, pairs.Source.OriginalBytes, incomplete, bytes, diagnostic));
	const std::array<PxcxStructureEdit, 1> complete{
		PxcxDynamicInputInsert{"format", 1, R"([{"r":{"d":"other"}},{"r":{"d":"second"}}])"}
	};
	CHECK(Graph(Edited(pairs, complete))["nodes"][0]["inputs"].size() == 5);
}

TEST_CASE(
	"PXC group source creation and explicit descendant deletion validate atomically",
	"[imagegraphio][pxcx_structure]"
) {
	const auto source = Imported();
	const std::array<PxcxStructureEdit, 2> add{
		PxcxNodeInsert{
			R"({"id":"group","type":"Node_Group","x":10,"y":20,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]},"future":{"keep":3}})"
		},
		PxcxNodeInsert{
			R"({"id":"child","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":42}}],"group":"group"})"
		}
	};
	const auto grouped = Edited(source, add);
	REQUIRE(grouped.Graph.Groups.size() == 1);
	const std::array<PxcxStructureEdit, 1> leaveChildren{PxcxNodeDelete{"group", false}};
	std::vector<std::byte> bytes{std::byte{44}};
	Diagnostic diagnostic;
	CHECK_FALSE(
		WritePxcxStructureEdits(grouped, grouped.Source.OriginalBytes, leaveChildren, bytes, diagnostic)
	);
	CHECK(bytes == std::vector<std::byte>{std::byte{44}});
	const std::array<PxcxStructureEdit, 1> remove{PxcxNodeDelete{"group", true}};
	const auto restored = Edited(grouped, remove);
	CHECK(Graph(restored) == Graph(source));
}

TEST_CASE(
	"PXC known source constructors create default inputs without guessing source values",
	"[imagegraphio][pxcx_structure]"
) {
	const auto source = Imported();
	const std::array<PxcxStructureEdit, 4> creates{
		PxcxNodeCreate{"created-number", "Node_Number_Simple", {3, 4}},
		PxcxNodeCreate{"created-array", "Node_Array_Pin", {4, 5}},
		PxcxNodeCreate{"created-format", "Node_String_Format", {5, 6}},
		PxcxNodeCreate{"created-group", "Node_Group", {6, 7}}
	};
	const auto result = Edited(source, creates);
	CHECK(result.Source.Nodes.size() == source.Source.Nodes.size() + creates.size());
	const auto json = Graph(result);
	CHECK(json["nodes"][3]["inputs"] == Json::array({Json::object()}));
	CHECK(json["nodes"][4]["inputs"].empty());
	CHECK(json["nodes"][5]["inputs"] == Json::array({Json::object()}));
	CHECK(json["nodes"][6]["version"] == 121092);
	const std::array<PxcxStructureEdit, 1> unknown{PxcxNodeCreate{"bad", "Node_NotInPinnedSource", {}}};
	std::vector<std::byte> bytes{std::byte{66}};
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxStructureEdits(source, source.Source.OriginalBytes, unknown, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{66}});
}

TEST_CASE(
	"PXC supplied project modifications pass native container and projection validation",
	"[imagegraphio][pxcx_structure][external]"
) {
	const char *directory = std::getenv("PXCX_EXTERNAL_FIXTURE_DIR");
	if (!directory || !*directory) {
		SUCCEED("set PXCX_EXTERNAL_FIXTURE_DIR for modified external project acceptance");
		return;
	}
	for (const auto name : std::array{
			 "Black-Hole_121092.pxc",
			 "Fire-Tornado_121092.pxc",
			 "Glass-Block-Refraction_121092.pxc",
			 "Ornate-Trim_121092.pxc",
			 "Spark-Bolt_121092.pxc"
		 }) {
		INFO(name);
		std::ifstream input(std::filesystem::path(directory) / name, std::ios::binary | std::ios::ate);
		REQUIRE(input.is_open());
		const auto count = input.tellg();
		REQUIRE(count > 0);
		REQUIRE(uint64_t(count) <= engine::bake::PxcxLimits::MaximumArchiveBytes);
		std::vector<std::byte> bytes(static_cast<size_t>(count));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(bytes.data()), count);
		REQUIRE(input.gcount() == count);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport source;
		REQUIRE(ImportPxcxImageGraph(archive, source, failure));
		const std::array<PxcxStructureEdit, 1> create{
			PxcxNodeCreate{"native-created-validation-node", "Node_Number_Simple", {123, -45}}
		};
		const auto modified = Edited(source, create);
		CHECK(modified.Source.Nodes.size() == source.Source.Nodes.size() + 1);
		const std::array<PxcxStructureEdit, 1> remove{PxcxNodeDelete{"native-created-validation-node"}};
		CHECK(Graph(Edited(modified, remove)) == Graph(source));
	}
}

TEST_CASE(
	"PXC native projection saves edited values positions connections and dynamic fields together",
	"[imagegraphio][pxcx_projection]"
) {
	const auto original = Imported();
	Document authored = original.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(authored, diagnostic) == Status::Ok);
	authored.Nodes.front().Position = {31, 42};
	auto value = std::find_if(
		authored.Nodes.front().Values.begin(), authored.Nodes.front().Values.end(), [](const auto &item) {
			return item.Port == "value";
		}
	);
	REQUIRE(value != authored.Nodes.front().Values.end());
	value->Data = 17.0;
	for (auto &key : authored.Keyframes)
		if (key.NodeId == "number" && key.Port == "value") key.Data = 17.0;
	authored.Links.clear();
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(original, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport reloaded;
	REQUIRE(ImportPxcxImageGraph(archive, reloaded, failure));
	REQUIRE(Migrate(reloaded.Graph, diagnostic) == Status::Ok);
	CHECK(reloaded.Graph == authored);
	CHECK(Graph(reloaded)["future"] == Graph(original)["future"]);
	CHECK(Graph(reloaded)["nodes"][2]["inputs"][0]["r"]["d"] == 9);
}

TEST_CASE(
	"PXC projection exact no-op and unsupported authored fields preserve output",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported();
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == source.Source.OriginalBytes);
	desired.Nodes.front().InstanceBase = "missing-source-instance-base";
	bytes = {std::byte{22}};
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{22}});
}

TEST_CASE(
	"PXC projection creates native catalogue nodes and writes their connected dynamic inputs",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported();
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	Node number;
	number.Id = "native-number";
	number.Type = FindCatalogueSource("Node_Number_Simple")->Type;
	number.Position = {20, 30};
	number.Values = {{"value", 9.0}};
	desired.Nodes.push_back(number);
	const auto *array = FindCatalogueSource("Node_Array_Pin");
	REQUIRE(array != nullptr);
	REQUIRE(array->DynamicTemplate.size() == 1);
	Node pin;
	pin.Id = "native-array";
	pin.Type = array->Type;
	pin.DynamicInputs.push_back(
		{std::string(array->DynamicTemplate.front().Id) + "_0",
		 array->DynamicTemplate.front().Type,
		 std::nullopt}
	);
	desired.Nodes.push_back(pin);
	desired.Links.push_back(
		{number.Id,
		 std::string(FindCatalogueSource("Node_Number_Simple")->Outputs.front().Id),
		 pin.Id,
		 pin.DynamicInputs.front().Id}
	);
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Source.Nodes.size() == source.Source.Nodes.size() + 2);
	CHECK(result.Source.Links.back().FromNode == number.Id);
	CHECK(result.Source.Links.back().ToNode == pin.Id);
	Document isolated = result.Graph;
	std::erase_if(isolated.Nodes, [&](const auto &node) { return node.Id != number.Id; });
	std::erase_if(isolated.Keyframes, [&](const auto &key) { return key.NodeId != number.Id; });
	std::erase_if(isolated.Tracks, [&](const auto &track) { return track.NodeId != number.Id; });
	isolated.Links.clear();
	isolated.Outputs = {
		{"number", number.Id, std::string(FindCatalogueSource("Node_Number_Simple")->Outputs.front().Id)}
	};
	Plan plan;
	REQUIRE(Compile(isolated, plan, diagnostic) == Status::Ok);
	EvaluatedValue evaluated;
	REQUIRE(EvaluateValue(isolated, plan, "number", {}, evaluated, diagnostic) == Status::Ok);
	CHECK(std::get<double>(evaluated.Data) == 9);
}

TEST_CASE(
	"PXC dynamic source value edits retain compact dormant keys and unknown record fields",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported();
	const auto *entry = FindCatalogueSource("Node_String_Merge");
	REQUIRE(entry != nullptr);
	REQUIRE(entry->DynamicTemplate.size() == 1);
	const std::string port = std::string(entry->DynamicTemplate.front().Id) + "_0";
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"text", port, std::string("changed")}};
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(Graph(result)["nodes"][1]["inputs"][0]["r"]["d"] == "changed");
	CHECK(Graph(result)["nodes"][1]["inputs"][0]["future"]["key"] == 8);
}

TEST_CASE(
	"PXC projection edits preserve opaque source group membership and future metadata",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":3}}]},{"id":"opaque","type":"future.node","x":0,"y":0,"group":"unknown-future-group","inputs":[],"future":{"node":[1,2,3]}}],"future":{"project":8}})"
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.front().Position = {11, 12};
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(Graph(result)["nodes"][1] == Graph(source)["nodes"][1]);
	CHECK(Graph(result)["future"] == Graph(source)["future"]);
}

TEST_CASE(
	"PXC projection writes project rendering attributes and preserves future project metadata",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":3}}]}],"attributes":{"surface_dimension":[32,32],"future":{"flags":[1,2,3]}}})"
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Project->SurfaceWidth = 64;
	desired.Project->SurfaceHeight = 48;
	desired.Project->Interpolation = 1;
	desired.Project->Oversample = 4;
	desired.Project->ColorDepth = 4;
	desired.Project->Shader3D = 1;
	desired.Project->Palette = {{1, 2, 3, 4}, {255, 128, 64, 255}};
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Project == desired.Project);
	CHECK(Graph(result)["attributes"]["future"] == Graph(source)["attributes"]["future"]);
	bytes = {std::byte{22}};
	desired.Project->Shader3D = 2;
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{22}});
}

TEST_CASE(
	"PXC Array Split variable outputs retain source indices through native projection",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"split","type":"Node_Array_Split","x":0,"y":0,"inputs":[{},{}],"attri":{"output_amount":4,"future":9}},{"id":"sink","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"from_node":"split","from_index":3}]}]})"
	);
	REQUIRE(source.Graph.Nodes.front().DynamicOutputs.size() == 3);
	CHECK(source.Graph.Nodes.front().DynamicOutputs.back().Id == "val_3");
	REQUIRE(source.Graph.Links.size() == 1);
	CHECK(source.Graph.Links.front().FromPort == "val_3");
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.front().DynamicOutputs.push_back({"val_4", ValueType::Any});
	desired.Links.front().FromPort = "val_4";
	std::vector<std::byte> bytes{std::byte{22}};
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC source inverse would change node values");
	CHECK(bytes == std::vector<std::byte>{std::byte{22}});
	bool updatedCount = false;
	for (auto &value : desired.Nodes.front().Values)
		if (value.Port == "attribute_output_amount") {
			value.Data = 5.0;
			updatedCount = true;
		}
	REQUIRE(updatedCount);
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes.front().DynamicOutputs == desired.Nodes.front().DynamicOutputs);
	CHECK(result.Graph.Links.front().FromPort == "val_4");
	CHECK(Graph(result)["nodes"][0]["attri"]["future"] == 9);
	bytes = {std::byte{22}};
	desired.Nodes.front().DynamicOutputs.front().Id = "gap";
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{22}});
}

TEST_CASE(
	"PXC projection creates source group controls and connects parent boundary sockets",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported();
	Json skeleton = {
		{"nodes",
		 Json::array(
			 {{{"id", "group-new"},
			   {"type", "Node_Group"},
			   {"x", 0},
			   {"y", 0},
			   {"inputs", Json::array({Json::object()})},
			   {"attri",
				{{"custom_input_list", {"group-in"}},
				 {"custom_output_list", {"group-out"}},
				 {"color_depth", 1},
				 {"interpolate", 0},
				 {"oversample", 0}}}},
			  {{"id", "group-in"},
			   {"type", "Node_Group_Input"},
			   {"group", "group-new"},
			   {"x", 0},
			   {"y", 0},
			   {"inputs", std::vector<Json>(16, Json::object())}},
			  {{"id", "group-out"},
			   {"type", "Node_Group_Output"},
			   {"group", "group-new"},
			   {"x", 1},
			   {"y", 0},
			   {"inputs", Json::array({Json{{"from_node", "group-in"}, {"from_index", 0}}})}}}
		 )}
	};
	const auto added = Imported(skeleton.dump());
	REQUIRE(added.Graph.Groups.size() == 1);
	REQUIRE(added.Graph.Groups.front().Ports.size() == 2);
	Document desired = source.Graph;
	desired.Nodes.insert(desired.Nodes.end(), added.Graph.Nodes.begin(), added.Graph.Nodes.end());
	desired.Groups = added.Graph.Groups;
	desired.Junctions = added.Graph.Junctions;
	desired.Links.insert(desired.Links.end(), added.Graph.Links.begin(), added.Graph.Links.end());
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Groups == desired.Groups);
	CHECK(result.Graph.Junctions == desired.Junctions);
	CHECK(Graph(result)["nodes"].back()["attri"]["custom_input_list"][0] == "group-in");
}

TEST_CASE(
	"PXC source display names remain explicit and rename without discarding future fields",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"named","type":"Node_Number_Simple","name":"Parameter speed","iname":"Node_Number_Simple_7","x":0,"y":0,"inputs":[{"r":{"d":3}}],"future":9},{"id":"unnamed","type":"Node_Number_Simple","x":1,"y":0,"inputs":[]}]})"
	);
	CHECK(source.Graph.Nodes[0].SourceDisplayName == "Parameter speed");
	CHECK(source.Graph.Nodes[0].SourceInternalName == "Node_Number_Simple_7");
	CHECK(source.Graph.Nodes[1].SourceInternalName.empty());
	CHECK(source.Graph.Nodes[1].SourceDisplayName.empty());
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes[0].SourceDisplayName = "Renamed speed";
	desired.Nodes[0].SourceInternalName = "Node_Number_Simple_8";
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes[0].SourceDisplayName == "Renamed speed");
	CHECK(result.Graph.Nodes[0].SourceInternalName == "Node_Number_Simple_8");
	CHECK(result.Graph.Nodes[1].SourceDisplayName.empty());
	CHECK(Graph(result)["nodes"][0]["future"] == 9);
}
TEST_CASE(
	"PXC dynamic room layer bindings survive inverse edits independently of input order",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"room","type":"Node_GMRoom","x":0,"y":0,"inputs":[{"attri":{"layerName":"foreground","future":42}}]}]})"
	);
	REQUIRE(source.Graph.Nodes[0].DynamicInputs.size() == 1);
	CHECK(source.Graph.Nodes[0].DynamicInputs[0].SourceLayerName == "foreground");
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes[0].DynamicInputs[0].SourceLayerName = "renamed";
	std::vector<std::byte> bytes;
	INFO(diagnostic.Message);
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes[0].DynamicInputs[0].SourceLayerName == "renamed");
	CHECK(Graph(result)["nodes"][0]["inputs"][0]["attri"]["future"] == 42);
}
TEST_CASE(
	"PXC project globals retain named controls and source editor metadata", "[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"global_node":{"inputs":[{"global_name":"speed","global_type":1,"global_disp":0,"global_s_range":[0,10],"global_s_step":0.01,"r":{"d":3},"future":7}]},"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":2}}]}]})"
	);
	REQUIRE_FALSE(source.Graph.ProjectGlobalNodeId.empty());
	const auto &global = source.Graph.Nodes.back();
	REQUIRE(global.Type == "pc.global_scope");
	REQUIRE(global.DynamicInputs.size() == 1);
	CHECK(global.DynamicInputs[0].Id == "speed");
	REQUIRE(global.DynamicInputs[0].Default);
	CHECK(std::get<double>(*global.DynamicInputs[0].Default) == 3);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.back().DynamicInputs[0].Default = 5.0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == desired.ProjectGlobalNodeId) key.Data = 5.0;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(std::get<double>(*result.Graph.Nodes.back().DynamicInputs[0].Default) == 5);
	CHECK(Graph(result)["global_node"]["inputs"][0]["future"] == 7);
	CHECK(Graph(result)["global_node"]["inputs"][0]["global_s_step"] == 0.01);
}
TEST_CASE(
	"PXC project global authoring adds verified controls and deletes the full scope",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"global_node":{"inputs":[{"global_name":"speed","global_type":1,"global_disp":0,"r":{"d":3}}]},"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":2}}]}]})"
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.back().DynamicInputs.push_back({"offset", ValueType::Vector2, Vector2{2, 4}});
	std::vector<std::byte> bytes;
	const bool added = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(added);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes.back().DynamicInputs[1].Id == "offset");
	CHECK(Graph(result)["global_node"]["inputs"][1]["global_type"] == 1);
	CHECK(Graph(result)["global_node"]["inputs"][1]["global_disp"] == 7);
	desired = source.Graph;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	const auto id = desired.ProjectGlobalNodeId;
	desired.ProjectGlobalNodeId.clear();
	std::erase_if(desired.Nodes, [&](const auto &v) { return v.Id == id; });
	std::erase_if(desired.Keyframes, [&](const auto &v) { return v.NodeId == id; });
	std::erase_if(desired.Tracks, [&](const auto &v) { return v.NodeId == id; });
	const bool deleted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(deleted);
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.ProjectGlobalNodeId.empty());
	CHECK(Graph(result)["global_node"]["inputs"].empty());
}
TEST_CASE(
	"PXC control expressions preserve disabled code and edit exact source input records",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":2},"global_key":"value + 3","global_use":false,"future":9}]}]})"
	);
	REQUIRE(source.Graph.Nodes[0].SourceInputExpressions.size() == 1);
	CHECK(source.Graph.Nodes[0].SourceInputExpressions[0].Code == "value + 3");
	CHECK_FALSE(source.Graph.Nodes[0].SourceInputExpressions[0].Enabled);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes[0].SourceInputExpressions[0].Code = "value * 4";
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes[0].SourceInputExpressions == desired.Nodes[0].SourceInputExpressions);
	CHECK(Graph(result)["nodes"][0]["inputs"][0]["future"] == 9);
}
TEST_CASE(
	"PXC Mesh Warp preserves bounded source pin, boundary and Puppet controls",
	"[imagegraphio][pxcx_projection]"
) {
	Json inputs = std::vector<Json>(11, Json::object());
	inputs.push_back({{"r", {{"d", Json::array({0, 1, 2, 3, 4, 5, 6})}}}});
	const auto source = Imported(
		Json{
			{"nodes",
			 Json::array({Json{
				 {"id", "warp"},
				 {"type", "Node_Mesh_Warp"},
				 {"x", 0},
				 {"y", 0},
				 {"inputs", inputs},
				 {"attri",
				  {{"pin", Json::array({1, 2})},
				   {"mesh_bound",
					Json::array({Json::array({0, 0}), Json::array({1, 0}), Json::array({0, 1})})},
				   {"future", 17}}}
			 }})}
		}.dump()
	);
	REQUIRE(source.Graph.Nodes[0].Type == "pc.mesh_warp");
	REQUIRE(source.Graph.Nodes[0].SourceProperties.size() == 2);
	REQUIRE(source.Graph.Nodes[0].DynamicInputs[0].Default);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	auto &control = std::get<ArrayValue>(*desired.Nodes[0].DynamicInputs[0].Default);
	control.Elements[1] = 7.0;
	std::get<ArrayValue>(desired.Nodes[0].SourceProperties[0].Data).Elements[0] = int64_t{3};
	std::get<ArrayValue>(desired.Nodes[0].SourceProperties[1].Data).Elements[0] = Vector2{.25, .5};

	for (auto &key : desired.Keyframes)
		if (key.NodeId == "warp" && key.Port == "control_point_0") key.Data = control;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Graph.Nodes[0].SourceProperties == desired.Nodes[0].SourceProperties);
	CHECK(Graph(result)["nodes"][0]["attri"]["future"] == 17);
	CHECK(Graph(result)["nodes"][0]["inputs"][11]["r"]["d"][1] == 7);
	CHECK(Graph(result)["nodes"][0]["attri"]["pin"][0] == 3);
	CHECK(Graph(result)["nodes"][0]["attri"]["mesh_bound"][0] == Json::array({.25, .5}));
}
TEST_CASE(
	"PXC export framerate attributes remain distinct from generic value units",
	"[imagegraphio][pxcx_projection]"
) {
	Json inputs = std::vector<Json>(27, Json::object());
	inputs[8] = {{"r", {{"d", 2}}}, {"unit", 0}, {"attri", {{"unit", 1}, {"future", 5}}}};
	const auto source = Imported(
		Json{
			{"nodes",
			 Json::array(
				 {Json{{"id", "export"}, {"type", "Node_Export"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		}.dump()
	);
	REQUIRE(source.Graph.Nodes[0].Type == "pc.export");
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	auto unit =
		std::find_if(desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [](const auto &v) {
			return v.Port == "framerate_unit";
		});
	REQUIRE(unit != desired.Nodes[0].Values.end());
	CHECK(std::get<EnumValue>(unit->Data).Value == 1);
	unit->Data = EnumValue{0};
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(Graph(result)["nodes"][0]["inputs"][8]["attri"]["unit"] == 0);
	CHECK(Graph(result)["nodes"][0]["inputs"][8]["unit"] == 0);
	CHECK(Graph(result)["nodes"][0]["inputs"][8]["attri"]["future"] == 5);
}

TEST_CASE(
	"PXC animated Puppet controls retain sampled arrays and inverse key edits",
	"[imagegraphio][pxcx_projection]"
) {
	Json inputs = std::vector<Json>(11, Json::object());
	inputs.push_back(
		{{"anim", true},
		 {"r",
		  Json::array(
			  {Json::array(
				   {Json::array({0, 0}),
					Json::array({0, 0, 2, 3, 4, 5, 6}),
					Json::array({0, 1}),
					Json::array({0, 0}),
					0,
					0,
					true,
					0}
			   ),
			   Json::array(
				   {Json::array({0, 10}),
					Json::array({0, 10, 2, 3, 4, 5, 6}),
					Json::array({0, 1}),
					Json::array({0, 0}),
					0,
					0,
					true,
					0}
			   )}
		  )}}
	);
	const auto source = Imported(
		Json{
			{"nodes",
			 Json::array(
				 {Json{{"id", "warp"}, {"type", "Node_Mesh_Warp"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		}.dump()
	);
	REQUIRE(source.Graph.Nodes[0].Type == "pc.mesh_warp");
	REQUIRE(source.Graph.Nodes[0].DynamicInputs.size() == 1);
	REQUIRE(source.Graph.Keyframes.size() == 2);
	REQUIRE(std::get<ArrayValue>(source.Graph.Keyframes[1].Data).Elements.size() == 7);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	std::get<ArrayValue>(desired.Keyframes[1].Data).Elements[1] = 12.0;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	REQUIRE(Migrate(result.Graph, diagnostic) == Status::Ok);
	CHECK(result.Graph == desired);
	inputs[11]["r"][1][1].erase(inputs[11]["r"][1][1].begin());
	const auto malformed = Imported(
		Json{
			{"nodes",
			 Json::array(
				 {Json{{"id", "warp"}, {"type", "Node_Mesh_Warp"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		}.dump()
	);
	CHECK(malformed.Graph.Nodes[0].Type != "pc.mesh_warp");
}
TEST_CASE(
	"PXC animation regions preserve ordered duplicate labels exact times and record extras",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[],"aRegion":[{"l":"same","c":197121,"fs":-2.5,"fe":4.25,"future":17},{"l":"same","c":16777215,"fs":5,"fe":9}]})"
	);
	REQUIRE(source.Graph.Project);
	REQUIRE(source.Graph.Project->AnimationRegions.size() == 2);
	CHECK(source.Graph.Project->AnimationRegions[0].Start == FrameTime{2, .5, true});
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Project->AnimationRegions[0].Label = "changed";
	desired.Project->AnimationRegions[1].End = {10, .5, false};
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	REQUIRE(Migrate(result.Graph, diagnostic) == Status::Ok);
	CHECK(result.Graph == desired);
	CHECK(Graph(result)["aRegion"][0]["future"] == 17);
	CHECK(Graph(result)["aRegion"][1]["fe"] == 10.5);
}

TEST_CASE(
	"PXC Lua argument sockets select the exact native carrier from source type controls",
	"[imagegraphio][pxcx_projection]"
) {
	Json inputs = std::vector<Json>(5, Json::object());
	inputs.push_back({{"r", {{"d", "message"}}}});
	inputs.push_back({{"r", {{"d", 1}}}});
	inputs.push_back({{"r", {{"d", "hello"}}}, {"attri", {{"future", 5}}}});
	const auto source = Imported(
		Json{
			{"nodes",
			 Json::array(
				 {Json{{"id", "lua"}, {"type", "Node_Lua_Compute"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		}.dump()
	);
	REQUIRE(source.Graph.Nodes[0].Type == "pc.lua_compute");
	REQUIRE(source.Graph.Nodes[0].DynamicInputs.size() == 3);
	CHECK(source.Graph.Nodes[0].DynamicInputs[2].Type == ValueType::Text);
	CHECK(std::get<std::string>(*source.Graph.Nodes[0].DynamicInputs[2].Default) == "hello");
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == source.Source.OriginalBytes);
}

TEST_CASE(
	"PXC native appended dynamic defaults receive exact source static bookkeeping",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"text","type":"Node_String_Merge","x":0,"y":0,"inputs":[{"r":{"d":"first"}}]}]})"
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes[0].DynamicInputs.push_back({"text_1", ValueType::Text, std::string("second")});
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	REQUIRE(result.Graph.Nodes[0].DynamicInputs.size() == 2);
	CHECK(std::get<std::string>(*result.Graph.Nodes[0].DynamicInputs[1].Default) == "second");
	CHECK(Graph(result)["nodes"][0]["inputs"][0] == Graph(source)["nodes"][0]["inputs"][0]);
}

TEST_CASE(
	"PXC Aseprite layer visibility and loop controls retain boolean metadata",
	"[imagegraphio][pxcx_projection]"
) {
	const auto source = Imported(
		R"({"nodes":[{"id":"ase","type":"Node_ASE_File_Read","x":0,"y":0,"inputs":[],"attri":{"layer_loop":[true,false],"layer_visible":[false,true],"future":5}}]})"
	);
	REQUIRE(source.Graph.Nodes[0].Type == "pc.ase_file_read");
	REQUIRE(source.Graph.Nodes[0].SourceProperties.size() == 1);
	const auto loop = std::find_if(
		source.Graph.Nodes[0].Values.begin(), source.Graph.Nodes[0].Values.end(), [](const auto &v) {
			return v.Port == "attribute_layer_loop";
		}
	);
	REQUIRE(loop != source.Graph.Nodes[0].Values.end());
	CHECK(std::get<ArrayValue>(loop->Data).ElementType == ValueType::Boolean);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	std::get<ArrayValue>(desired.Nodes[0].SourceProperties[0].Data).Elements[0] = true;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport result;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(Graph(result)["nodes"][0]["attri"]["layer_visible"][0] == true);
	CHECK(Graph(result)["nodes"][0]["attri"]["layer_loop"][0] == true);
	CHECK(Graph(result)["nodes"][0]["attri"]["future"] == 5);
}
