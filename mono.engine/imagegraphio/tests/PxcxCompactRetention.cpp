#include "CompactSourceAnimatorEdit.hpp"

#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_compact_retention")

namespace {
	using Json = nlohmann::ordered_json;
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;

	PxcxImport Imported(const Json &root) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = root.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}

	Json Saved(const PxcxImport &source, const Document &desired) {
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport reopened;
		REQUIRE(ImportPxcxImageGraph(archive, reopened, failure));
		CHECK(reopened.Graph == desired);
		return Json::parse(std::string_view(archive.GraphJson.data(), archive.GraphJson.size() - 1));
	}
}

TEST_CASE("PXC global default edits retain compact animator extension fields", "[pxcx_compact_retention]") {
	const Json root = Json::parse(
		R"({"global_node":{"inputs":[{"global_name":"speed","global_type":1,"global_disp":0,"r":{"d":3,"future":{"id":"durable-id","values":[1,2]}},"future_input":7}]},"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":2}}]}],"future_project":true})"
	);
	const auto source = Imported(root);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.back().DynamicInputs[0].Default = 5.0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == desired.ProjectGlobalNodeId) key.Data = 5.0;
	const auto saved = Saved(source, desired);
	auto expected = root["global_node"]["inputs"][0]["r"];
	expected["d"] = 5.0;
	CHECK(saved["global_node"]["inputs"][0]["r"] == expected);
	CHECK(saved["global_node"]["inputs"][0]["future_input"] == 7);
	CHECK(saved["future_project"] == true);
}

TEST_CASE("PXC Puppet value edits retain compact animator extension fields", "[pxcx_compact_retention]") {
	Json inputs = std::vector<Json>(11, Json::object());
	inputs.push_back({{"r", {{"d", Json::array({0, 1, 2, 3, 4, 5, 6})}, {"future", "keep"}}}});
	const auto source = Imported(
		{{"nodes",
		  Json::array(
			  {Json{{"id", "warp"}, {"type", "Node_Mesh_Warp"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
		  )}}
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	REQUIRE(desired.Nodes[0].DynamicInputs[0].Default);
	auto &control = std::get<ArrayValue>(*desired.Nodes[0].DynamicInputs[0].Default);
	control.Elements[1] = 7.0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "warp" && key.Port == "control_point_0") key.Data = control;
	const auto saved = Saved(source, desired);
	CHECK(saved["nodes"][0]["inputs"][11]["r"]["d"][1] == 7);
	CHECK(saved["nodes"][0]["inputs"][11]["r"]["future"] == "keep");
}

TEST_CASE(
	"Compact animator editing refuses malformed and expanded records without mutation",
	"[pxcx_compact_retention]"
) {
	for (Json input :
		 {Json::array(),
		  Json{{"r", nullptr}},
		  Json{{"r", 3}},
		  Json{{"r", Json::array({Json::array({0, 3})})}},
		  Json{{"r", {{"future", 7}}}}}) {
		const auto before = input;
		CHECK_FALSE(engine::imagegraphio::detail::WriteCompactSourceAnimatorValue(input, 5));
		CHECK(input == before);
	}
	Json input = {{"future_input", "keep"}};
	REQUIRE(engine::imagegraphio::detail::WriteCompactSourceAnimatorValue(input, 5));
	CHECK((input == Json{{"future_input", "keep"}, {"r", {{"d", 5}}}}));
}
TEST_CASE(
	"PXC Puppet rejects an alternate source-property representation atomically", "[pxcx_compact_retention]"
) {
	Json inputs = std::vector<Json>(11, Json::object());
	inputs.push_back({{"r", {{"d", Json::array({0, 1, 2, 3, 4, 5, 6})}, {"future", "keep"}}}});
	const auto source = Imported(
		{{"nodes",
		  Json::array(
			  {Json{{"id", "warp"}, {"type", "Node_Mesh_Warp"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
		  )}}
	);
	Document desired = source.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	REQUIRE(desired.Nodes[0].DynamicInputs[0].Default);
	auto control = std::get<ArrayValue>(*desired.Nodes[0].DynamicInputs[0].Default);
	control.Elements[1] = 7.0;
	desired.Nodes[0].SourceProperties.push_back({"control_point_0", control});
	const std::vector<std::byte> sentinel{std::byte{0x51}, std::byte{0x72}};
	auto bytes = sentinel;
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(source.Source.GraphJson.find("keep") != std::string::npos);
}
