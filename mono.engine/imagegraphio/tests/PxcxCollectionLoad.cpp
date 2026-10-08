#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_collection_load")
TEST_DEPENDS("engine.imagegraphio.pxcximport")

namespace {
	using Json = nlohmann::json;
	using namespace engine::imagegraphio;
	using engine::bake::PxcxArchive;
	using engine::imagegraph::Diagnostic;

	Json Expanded(Json marker, Json value, Json tail = "key-tail") {
		return Json::array(
			{std::move(marker),
			 std::move(value),
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 std::move(tail)}
		);
	}

	Json Animated(std::string label) {
		return Json{
			{"anim", true},
			{"on_end", 0},
			{"raw_value", Json::array({Expanded(Json::array({0, 0.0625}), 50, label + "-raw-tail")})},
			{"r",
			 Json::array(
				 {Expanded(Json::array({0, 0.0625, "marker-tail"}), 10, label + "-zero"),
				  Expanded(Json::array({1, 0.1875, "mode-tail"}), 20, label + "-two"),
				  Expanded(-0.0625, 30, label + "-negative-zero"),
				  Expanded(Json::array({0, -0.1875}), 40, label + "-negative-two")}
			 )},
			{"animators",
			 Json::array(
				 {Json::array(
					  {Expanded(Json::array({0, 0.0625, "axis-marker-tail"}), 1, "axis-positive-tail")}
				  ),
				  Json::array({Expanded(-0.1875, 2, "axis-negative-tail")})}
			 )}
		};
	}

	Json SourceGraph() {
		Json collection = {
			{"id", "root"},
			{"type", "Node_Collection"},
			{"x", 0},
			{"y", 0},
			{"inputs", Json::array()},
			{"attri", {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}}}
		};
		Json number = {
			{"id", "number"},
			{"type", "Node_Number_Simple"},
			{"x", 3},
			{"y", 4},
			{"group", "root"},
			{"inputs", Json::array({Animated("input")})},
			{"raw_value", Animated("raw")},
			{"outputs", Json::array({Animated("output")})},
			{"outputMeta", Animated("output-meta")},
			{"inspectInputs",
			 Json::array(
				 {Animated("inspect-0"),
				  Animated("inspect-1"),
				  Animated("inspect-2"),
				  Animated("inspect-3"),
				  Animated("inspect-4"),
				  Animated("inspect-5")}
			 )}
		};
		// Update has Boolean Trigger keys; retain the generic fixture's source clocks and opaque tails.
		for (auto &key : number["inspectInputs"][2]["r"])
			key[1] = false;
		Json compact = {
			{"id", "compact"},
			{"type", "Node_Number_Simple"},
			{"x", 5},
			{"y", 6},
			{"group", "root"},
			{"inputs", Json::array({Json{{"r", {{"d", 17}}}}})}
		};
		return Json{
			{"version", 121092},
			{"versionStr", "1.21.10.203"},
			{"animator", {{"frames_total", 99}, {"framerate", 24}, {"playback", 1}}},
			{"metadata", {{"same", "source"}, {"source_only", true}}},
			{"nodes", Json::array({collection, number, compact})}
		};
	}

	PxcxArchive Archive(const Json &graph) {
		PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = graph.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}

	PxcxCollectionSave Files(Json graph, std::optional<std::string> metadata = std::nullopt) {
		return {graph.dump(), std::move(metadata)};
	}

	Json ArchiveGraph(const PxcxArchive &archive) {
		return Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	}

	PxcxArchive Load(
		const PxcxCollectionSave &files,
		engine::imagegraph::TimelineSettings timeline = {
			.Frames = 9, .Last = 8, .Playback = "loop", .FramesPerSecond = 24.0
		}
	) {
		PxcxArchive archive;
		Diagnostic diagnostic;
		const bool loaded = PreparePxcxCollectionLoad(files, timeline, archive, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(loaded);
		return archive;
	}
}

TEST_CASE(
	"PXC Collection load scales expanded input keys with banker rounding and keeps tails",
	"[imagegraphio][collection_load][animation]"
) {
	const auto files = Files(SourceGraph());
	const auto original = files.GraphJson;
	const auto archive = Load(files);
	const auto saved = ArchiveGraph(archive);
	const auto &number = saved["nodes"][1];
	const auto &keys = number["inputs"][0]["r"];
	REQUIRE(keys.size() == 4);
	CHECK(keys[0][0][0] == 0);
	CHECK(keys[0][0][1] == 0);
	CHECK(keys[0][0][2] == "marker-tail");
	CHECK(keys[0][8] == "input-zero");
	CHECK(keys[1][0][0] == 1);
	CHECK(keys[1][0][1] == 2);
	CHECK(keys[1][0][2] == "mode-tail");
	CHECK(keys[1][8] == "input-two");
	CHECK(keys[2][0] == 0);
	CHECK(keys[2][8] == "input-negative-zero");
	CHECK(keys[3][0][1] == -2);
	CHECK(keys[3][8] == "input-negative-two");
	CHECK(number["inputs"][0]["raw_value"][0][0][1] == 0);
	CHECK(number["inputs"][0]["raw_value"][0][8] == "input-raw-tail");
	CHECK(number["inputs"][0]["animators"] == SourceGraph()["nodes"][1]["inputs"][0]["animators"]);
	CHECK(number["outputs"] == SourceGraph()["nodes"][1]["outputs"]);
	CHECK(number["outputMeta"] == SourceGraph()["nodes"][1]["outputMeta"]);
	for (const size_t index : {size_t{0}, size_t{1}, size_t{2}, size_t{4}}) {
		CHECK(number["inspectInputs"][index]["r"][0][0][1] == 0);
		CHECK(
			number["inspectInputs"][index]["animators"] ==
			SourceGraph()["nodes"][1]["inspectInputs"][index]["animators"]
		);
	}
	CHECK(number["inspectInputs"][4]["r"][1][0][1] == 2);
	CHECK(number["inspectInputs"][3] == SourceGraph()["nodes"][1]["inspectInputs"][3]);
	CHECK(number["inspectInputs"][5] == SourceGraph()["nodes"][1]["inspectInputs"][5]);
	CHECK(saved["animator"]["frames_total"] == 9);
	CHECK(saved["animator"]["framerate"] == 24.0);
	CHECK(saved["animator"]["playback"] == 0);
	CHECK((saved["nodes"][2]["inputs"][0]["r"] == Json{{"d", 17}}));
	CHECK(files.GraphJson == original);
	CHECK(archive.MetadataNumber == 121092);

	PxcxImport imported;
	std::string failure;
	const bool accepted = ImportPxcxImageGraph(archive, imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	const auto common = std::find_if(
		imported.Graph.SourceCommonOwners.begin(),
		imported.Graph.SourceCommonOwners.end(),
		[](const auto &owner) { return owner.SourceOwnerId == "number"; }
	);
	REQUIRE(common != imported.Graph.SourceCommonOwners.end());
	REQUIRE(bool(imported.Graph.SourceAnimators));
	const auto update = std::find_if(
		imported.Graph.SourceAnimators->DetachedValues.begin(),
		imported.Graph.SourceAnimators->DetachedValues.end(),
		[&](const auto &value) {
			return value.NodeId == "number" && value.Port == common->UpdateAnimatorPort;
		}
	);
	REQUIRE(update != imported.Graph.SourceAnimators->DetachedValues.end());
	REQUIRE(update->Keys.size() == 4);
	CHECK(update->Keys[2].Kind == engine::imagegraph::KeyframeKind::Normal);
	CHECK(engine::imagegraph::GetFrameTime(update->Keys[2]).Tick == 0);
	CHECK(update->Keys[3].Kind == engine::imagegraph::KeyframeKind::Normal);
	CHECK(engine::imagegraph::GetFrameTime(update->Keys[3]).NegativeFrame);
	CHECK(engine::imagegraph::GetFrameTime(update->Keys[3]).Tick == 2);
	for (const auto &key : update->Keys)
		CHECK(key.Data == engine::imagegraph::Value{false});
	REQUIRE(imported.Graph.Groups.size() == 1);
	CHECK(imported.Graph.Groups.front().Id == "root");
	const auto child =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "number";
		});
	REQUIRE(child != imported.Graph.Nodes.end());
	CHECK(child->GroupId == "root");
}

TEST_CASE(
	"PXC Collection load replaces inline metadata with its explicit sidecar",
	"[imagegraphio][collection_load]"
) {
	auto timeline = engine::imagegraph::TimelineSettings{};
	timeline.Frames = 9;
	timeline.Last = 8;
	timeline.Playback = "pingpong";
	const auto files = Files(SourceGraph(), R"JSON({"same":"sidecar","sidecar_only":{"keep":17}})JSON");
	const auto archive = Load(files, timeline);
	const auto saved = ArchiveGraph(archive);
	CHECK(saved["metadata"]["same"] == "sidecar");
	CHECK_FALSE(saved["metadata"].contains("source_only"));
	CHECK(saved["metadata"]["sidecar_only"]["keep"] == 17);
	CHECK(saved["animator"]["playback"] == 2);
}

TEST_CASE(
	"PXC Collection load applies root manager overrides only to top-level minus-four collections",
	"[imagegraphio][collection_load][metadata]"
) {
	auto graph = SourceGraph();
	graph["nodes"][0]["group"] = -4;
	graph["nodes"].push_back(
		{{"id", "nested"},
		 {"type", "Node_Collection"},
		 {"x", 10},
		 {"y", 11},
		 {"group", "root"},
		 {"inputs", Json::array()},
		 {"attri", {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}}}}
	);
	for (const auto &[id, parent] :
		 {std::pair{"root-null", Json(nullptr)}, std::pair{"root-empty", Json("")}}) {
		graph["nodes"].push_back(
			{{"id", id},
			 {"type", "Node_Collection"},
			 {"x", 20},
			 {"y", 21},
			 {"group", parent},
			 {"inputs", Json::array()},
			 {"attri", {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}}}}
		);
	}
	graph["metadata"] = {
		{"description", "root description"},
		{"author", "root author"},
		{"contact", "root contact"},
		{"version", 9},
		{"tags", Json::array({"source-tag"})}
	};
	const auto archive = Archive(graph);
	const auto originalGraph = archive.GraphJson;
	std::vector<engine::imagegraphio::PxcxCollectionMetadata> managers;
	Diagnostic diagnostic;
	REQUIRE(PreparePxcxCollectionMetadata(
		archive, managers, diagnostic, engine::imagegraph::Limits::MaximumEvaluationBytes, true
	));
	REQUIRE(managers.size() == 4);
	CHECK(managers[0].NodeId == "root");
	const auto root = Json::parse(managers[0].MetadataJson);
	CHECK(root["description"] == "root description");
	CHECK(root["author"] == "root author");
	CHECK(root["contact"] == "root contact");
	CHECK(root["version"] == 9);
	CHECK(root["tags"] == Json::array({"source-tag"}));
	CHECK(managers[1].NodeId == "nested");
	const auto child = Json::parse(managers[1].MetadataJson);
	CHECK(child["description"] == "");
	CHECK(child["author"] == "");
	CHECK(child["contact"] == "");
	CHECK(child["version"] == 121092);
	CHECK(child["tags"].empty());
	CHECK(managers[2].NodeId == "root-null");
	CHECK(Json::parse(managers[2].MetadataJson) == root);
	CHECK(managers[3].NodeId == "root-empty");
	CHECK(Json::parse(managers[3].MetadataJson) == root);
	CHECK(archive.GraphJson == originalGraph);

	std::vector<engine::imagegraphio::PxcxCollectionMetadata> defaults;
	REQUIRE(PreparePxcxCollectionMetadata(archive, defaults, diagnostic));
	REQUIRE(defaults.size() == 4);
	CHECK(Json::parse(defaults[0].MetadataJson) == child);
	CHECK(Json::parse(defaults[1].MetadataJson) == child);
	CHECK(Json::parse(defaults[2].MetadataJson) == child);
	CHECK(Json::parse(defaults[3].MetadataJson) == child);
}

TEST_CASE(
	"PXC append treats minus-four source parent as top-level within destination context",
	"[imagegraphio][collection_load][append]"
) {
	auto incomingGraph = SourceGraph();
	incomingGraph["nodes"][0]["group"] = -4;
	const auto incoming = Archive(incomingGraph);
	const auto destination = Archive(
		Json{
			{"nodes",
			 Json::array({Json{
				 {"id", "destinationCollection"},
				 {"type", "Node_Collection"},
				 {"x", 0},
				 {"y", 0},
				 {"inputs", Json::array()},
				 {"attri", {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}}}
			 }})}
		}
	);
	PxcxAppendOptions options;
	options.Namespace = "loaded";
	options.Context = "destinationCollection";
	PxcxAppendResult result;
	Diagnostic diagnostic;
	const bool appended = AppendPxcxProject(destination, incoming, options, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(appended);
	REQUIRE(result.Nodes.size() == 3);
	CHECK(result.Nodes[0] == (PxcxAppendedNode{"root", "loaded/root", true}));
	CHECK(result.Nodes[1] == (PxcxAppendedNode{"number", "loaded/number", false}));
	const auto saved = ArchiveGraph(result.Project.Source);
	const auto root = std::find_if(saved["nodes"].begin(), saved["nodes"].end(), [](const auto &node) {
		return node["id"] == "loaded/root";
	});
	REQUIRE(root != saved["nodes"].end());
	CHECK((*root)["group"] == "destinationCollection");
	const auto number = std::find_if(saved["nodes"].begin(), saved["nodes"].end(), [](const auto &node) {
		return node["id"] == "loaded/number";
	});
	REQUIRE(number != saved["nodes"].end());
	CHECK((*number)["group"] == "loaded/root");
}

TEST_CASE(
	"PXC Collection load maps every animator time to zero for one-frame projects",
	"[imagegraphio][collection_load][animation]"
) {
	engine::imagegraph::TimelineSettings timeline;
	timeline.Frames = 1;
	const auto archive = Load(Files(SourceGraph()), timeline);
	const auto saved = ArchiveGraph(archive);
	const auto &keys = saved["nodes"][1]["inputs"][0]["r"];
	REQUIRE(keys.size() == 4);
	CHECK(keys[0][0][1] == 0);
	CHECK(keys[1][0][1] == 0);
	CHECK(keys[2][0] == 0);
	CHECK(keys[3][0][1] == 0);
	CHECK(saved["animator"]["frames_total"] == 1);
}

TEST_CASE(
	"PXC Collection load scales separated axes only when the source toggle is enabled",
	"[imagegraphio][collection_load][animation]"
) {
	auto graph = SourceGraph();
	auto &number = graph["nodes"][1];
	number["type"] = "Node_Vector2";
	auto &input = number["inputs"][0];
	input["sep_axis"] = true;
	input["r"] = Json::array({Expanded(Json::array({0, 0.0625}), Json::array({2, 4}), "vector-tail")});
	const auto archive = Load(Files(std::move(graph)));
	const auto saved = ArchiveGraph(archive);
	const auto &exported = saved["nodes"][1]["inputs"][0];
	CHECK(exported["r"][0][0][1] == 0);
	CHECK(exported["r"][0][8] == "vector-tail");
	CHECK(exported["animators"][0][0][0][1] == 0);
	CHECK(exported["animators"][0][0][8] == "axis-positive-tail");
	CHECK(exported["animators"][1][0][0] == -2);
	CHECK(exported["animators"][1][0][8] == "axis-negative-tail");
}

TEST_CASE(
	"PXC Collection load refuses invalid sources without changing the archive",
	"[imagegraphio][collection_load][atomic]"
) {
	PxcxArchive prior;
	prior.MetadataNumber = 73;
	prior.MetadataText = "previous";
	prior.GraphJson = "previous graph";
	engine::imagegraph::TimelineSettings timeline;
	timeline.Frames = 9;
	timeline.Last = 8;
	const std::string duplicateJson = R"JSON({
		"version":121092,
		"version":121092,
		"versionStr":"1.21.10.203",
		"nodes":[{"id":"root","type":"Node_Collection","x":0,"y":0,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]}}]
	})JSON";
	for (const auto &files : {
			 PxcxCollectionSave{"", std::nullopt},
			 PxcxCollectionSave{"{bad json", std::nullopt},
			 PxcxCollectionSave{duplicateJson, std::nullopt},
		 }) {
		PxcxArchive output = prior;
		Diagnostic diagnostic;
		CHECK_FALSE(PreparePxcxCollectionLoad(files, timeline, output, diagnostic));
		CHECK(output.MetadataNumber == prior.MetadataNumber);
		CHECK(output.MetadataText == prior.MetadataText);
		CHECK(output.GraphJson == prior.GraphJson);
	}
	for (auto invalidGraph :
		 {Json{{"versionStr", "1.21.10.203"}, {"nodes", Json::array()}},
		  Json{{"version", 121092}, {"nodes", Json::array()}},
		  Json{{"version", 121093}, {"versionStr", "future"}, {"nodes", Json::array()}},
		  Json{{"version", 121092}, {"versionStr", "1.21.10.203"}}}) {
		PxcxArchive output = prior;
		Diagnostic diagnostic;
		CHECK_FALSE(PreparePxcxCollectionLoad(Files(invalidGraph), timeline, output, diagnostic));
		CHECK(output.GraphJson == prior.GraphJson);
	}
	PxcxArchive output = prior;
	Diagnostic diagnostic;
	timeline.Frames = 0;
	CHECK_FALSE(PreparePxcxCollectionLoad(Files(SourceGraph()), timeline, output, diagnostic));
	CHECK(output.GraphJson == prior.GraphJson);
	timeline.Frames = 9;
	CHECK_FALSE(PreparePxcxCollectionLoad(Files(SourceGraph()), timeline, output, diagnostic, 1));
	CHECK(output.GraphJson == prior.GraphJson);
	timeline.Playback = "unknown";
	CHECK_FALSE(PreparePxcxCollectionLoad(Files(SourceGraph()), timeline, output, diagnostic));
	CHECK(output.GraphJson == prior.GraphJson);
}
