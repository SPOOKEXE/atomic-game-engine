#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/NoiseField.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_modified_project")
TEST_DEPENDS("engine.imagegraphio.pxcx_noise_field")
TEST_DEPENDS("engine.imagegraphio.pxcx_group_values")
TEST_DEPENDS("engine.imagegraphio.pxcx_compact_retention")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;

	Json NodeRecord(std::string id, std::string type, Json inputs = Json::array()) {
		return {
			{"id", std::move(id)},
			{"type", std::move(type)},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Project() {
		const auto *noise = FindCatalogueEntry("pc.fold_noise");
		REQUIRE(noise);
		const Json physicalInputs = std::vector<Json>(14, Json::object());
		auto base = NodeRecord("noise-base", std::string(noise->SourceNode), physicalInputs);
		base["atomic_game_engine"] = {
			{"version", 1},
			{"future_engine", 17},
			{"noise_field", {{"output_type", 3}, {"future_field", "keep"}}}
		};
		auto instance = NodeRecord("noise-instance", std::string(noise->SourceNode), physicalInputs);
		instance["instanceBase"] = "noise-base";
		instance["atomic_game_engine"] = {
			{"version", 1}, {"noise_field", {{"output_type", 2}, {"future_field", "instance"}}}
		};
		Json parent = {
			{"anim", true},
			{"future_input", 23},
			{"r",
			 Json::array(
				 {Json::array(
					  {Json::array({0, 2}),
					   false,
					   Json::array({0, 1}),
					   Json::array({0, 0}),
					   0,
					   0,
					   true,
					   0,
					   16777215,
					   Json{{"physical_identity", "first"}}}
				  ),
				  Json::array(
					  {Json::array({0, 5}),
					   true,
					   Json::array({0, 1}),
					   Json::array({0, 0}),
					   0,
					   0,
					   true,
					   0,
					   16777215,
					   Json{{"physical_identity", "second"}}}
				  )}
			 )}
		};
		auto group = NodeRecord("group", "Node_Group", Json::array({parent}));
		group["attri"] = {
			{"custom_input_list", {"input"}},
			{"custom_output_list", {"output"}},
			{"color_depth", 1},
			{"interpolate", 0},
			{"oversample", 0}
		};
		Json controls = std::vector<Json>(16, Json::object());
		controls[0] = {{"r", {{"d", 0}}}};
		controls[1] = {{"r", {{"d", {0, 10}}}}};
		controls[2] = {{"r", {{"d", 19}}}};
		auto input = NodeRecord("input", "Node_Group_Input", controls);
		input["group"] = "group";
		auto output = NodeRecord(
			"output",
			"Node_Group_Output",
			Json::array({Json{{"from_node", "input"}, {"from_index", 0}, {"from_tag", 0}}})
		);
		output["group"] = "group";
		return {
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", Json::array({1, "keep"})},
			{"global_node",
			 {{"inputs",
			   Json::array({Json{
				   {"global_name", "speed"},
				   {"global_type", 1},
				   {"global_disp", 0},
				   {"future_input", 7},
				   {"r", {{"d", 3}, {"future_compact", {{"durable", "global"}}}}}
			   }})}}},
			{"nodes", Json::array({base, instance, group, input, output})}
		};
	}
	PxcxImport Reimport(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		const bool accepted = ImportPxcxImageGraph(archive, imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Imported(const Json &project) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		return Reimport(bytes);
	}
	Node &NativeNode(Document &document, std::string_view id) {
		const auto node =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
				return candidate.Id == id;
			});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const Json &SourceNode(const Json &project, std::string_view id) {
		const auto &nodes = project.at("nodes");
		const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto &candidate) {
			return candidate.at("id") == id;
		});
		REQUIRE(node != nodes.end());
		return *node;
	}
	Document Modified(const PxcxImport &source) {
		auto desired = source.Graph;
		Diagnostic diagnostic;
		REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
		NativeNode(desired, "noise-instance").InstanceOverrides.push_back("output_type");
		NativeNode(desired, "noise-base").Position = {12, -4};
		auto &global = NativeNode(desired, desired.ProjectGlobalNodeId);
		REQUIRE(global.DynamicInputs.size() == 1);
		global.DynamicInputs[0].Default = 5.0;
		unsigned editedGroupKeys = 0;
		for (auto &key : desired.Keyframes) {
			if (key.NodeId == global.Id) key.Data = 5.0;
			if (key.NodeId == "input" && key.Port == "parent_value" && key.Tick == 2) {
				REQUIRE_FALSE(key.SourceKeyId.empty());
				key.Tick = 7;
				key.Data = true;
				++editedGroupKeys;
			}
		}
		REQUIRE(editedGroupKeys == 1);
		return desired;
	}
	Document Semantic(Document document) {
		// grug source ids locate records in one archive; changed clocks may mint fresh ids on reimport.
		for (auto &key : document.Keyframes)
			key.SourceKeyId.clear();
		std::sort(
			document.Keyframes.begin(), document.Keyframes.end(), [](const auto &left, const auto &right) {
				return std::tie(left.NodeId, left.Port, left.NegativeFrame, left.Tick, left.Subframe) <
					   std::tie(right.NodeId, right.Port, right.NegativeFrame, right.Tick, right.Subframe);
			}
		);
		return document;
	}
}

TEST_CASE(
	"Compound PXC edits retain native noise inheritance compact extensions and group key records",
	"[imagegraphio][pxcx_modified_project]"
) {
	const auto project = Project();
	const auto source = Imported(project);
	const auto originalGraph = source.Graph;
	const auto originalBytes = source.Source.OriginalBytes;
	const auto desired = Modified(source);
	Document nativeReload;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(desired), nativeReload, diagnostic) == Status::Ok);
	CHECK(nativeReload == desired);
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(source, nativeReload, {}, bytes, diagnostic);
	INFO(diagnostic.Message << " " << diagnostic.NodeId << " " << diagnostic.Port);
	REQUIRE(accepted);
	auto reopened = Reimport(bytes);
	REQUIRE(Migrate(reopened.Graph, diagnostic) == Status::Ok);
	CHECK(Semantic(reopened.Graph) == Semantic(desired));
	CHECK(RasterNoiseComponents(NativeNode(reopened.Graph, "noise-instance"), &reopened.Graph) == 2);
	CHECK(RasterNoiseComponents(NativeNode(reopened.Graph, "noise-base"), &reopened.Graph) == 3);
	const auto saved = Json::parse(reopened.Source.GraphJson.c_str());
	CHECK(saved["future_project"] == project["future_project"]);
	for (std::string_view id : {"noise-base", "noise-instance"}) {
		CHECK(SourceNode(saved, id)["inputs"] == SourceNode(project, id)["inputs"]);
		CHECK(SourceNode(saved, id)["future_node"] == "keep");
		CHECK(
			SourceNode(saved, id)["atomic_game_engine"]["noise_field"]["future_field"] ==
			SourceNode(project, id)["atomic_game_engine"]["noise_field"]["future_field"]
		);
	}
	CHECK(SourceNode(saved, "noise-base")["atomic_game_engine"]["future_engine"] == 17);
	CHECK(SourceNode(saved, "noise-instance")["instanceBase"] == "noise-base");
	CHECK(
		SourceNode(saved, "noise-instance")["atomic_game_engine"]["noise_field"]["override_instance"] == true
	);
	auto expectedGlobal = project["global_node"];
	expectedGlobal["inputs"][0]["r"]["d"] = 5.0;
	CHECK(saved["global_node"] == expectedGlobal);
	const auto &parent = SourceNode(saved, "group")["inputs"][0];
	CHECK(parent["future_input"] == 23);
	REQUIRE(parent["r"].size() == 2);
	for (const auto &original : SourceNode(project, "group")["inputs"][0]["r"]) {
		const auto record = std::find_if(parent["r"].begin(), parent["r"].end(), [&](const auto &candidate) {
			return candidate.at(9) == original.at(9);
		});
		REQUIRE(record != parent["r"].end());
		auto expected = original;
		if (expected[9]["physical_identity"] == "first") {
			expected[0][1] = 7;
			expected[1] = true;
		}
		CHECK(*record == expected);
	}
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, diagnostic));
	CHECK(unchanged == bytes);
	CHECK(source.Graph == originalGraph);
	CHECK(source.Source.OriginalBytes == originalBytes);
}

TEST_CASE(
	"An unsupported member refuses a compound PXC transaction without publishing partial edits",
	"[imagegraphio][pxcx_modified_project]"
) {
	const auto source = Imported(Project());
	const auto originalGraph = source.Graph;
	const auto originalBytes = source.Source.OriginalBytes;
	auto desired = Modified(source);
	NativeNode(desired, "noise-instance").Values.push_back({"unsupported_future_socket", 1.0});
	const auto retainedDesired = desired;
	const std::vector<std::byte> sentinel{std::byte{0x61}, std::byte{0x62}};
	auto bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(bytes == sentinel);
	CHECK(desired == retainedDesired);
	CHECK(source.Graph == originalGraph);
	CHECK(source.Source.OriginalBytes == originalBytes);
}
