#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_collection_alias_append")
TEST_DEPENDS("engine.imagegraphio.pxcx_collection_instances")

namespace {
	using Json = nlohmann::json;
	using namespace engine::imagegraphio;

	Json Value(Json data) {
		return {{"r", {{"d", std::move(data)}}}};
	}
	Json Node(std::string id, std::string type, Json inputs = Json::array()) {
		return {{"id", std::move(id)}, {"type", std::move(type)}, {"x", 0}, {"y", 0}, {"inputs", inputs}};
	}
	Json Wire(std::string node) {
		return {{"from_node", std::move(node)}, {"from_index", 0}, {"from_tag", 0}};
	}
	engine::bake::PxcxArchive Archive(const Json &graph) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = graph.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	Json Source(bool nested) {
		auto outer = Node("outer", "Node_Group");
		auto copy = Node("copy", "Node_Group");
		copy["instanceBase"] = "outer";
		if (nested) {
			outer["group"] = "container";
			copy["group"] = "container";
		}
		auto baseCollection = Node("collection", "Node_Collection", Json::array({Value(10.0)}));
		baseCollection["group"] = "outer";
		baseCollection["attri"] = {
			{"custom_input_list", {"source-input"}}, {"custom_output_list", Json::array()}
		};
		auto localCollection = Node("local", "Node_Collection", Json::array({Value(99.0)}));
		localCollection["group"] = "copy";
		localCollection["attri"] = {
			{"custom_input_list", {"local-input"}}, {"custom_output_list", Json::array()}
		};
		auto sourceInput = Node(
			"source-input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(1)})
		);
		sourceInput["group"] = "collection";
		auto localInput = Node(
			"local-input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 2})), Value(1)})
		);
		localInput["group"] = "local";
		auto sourceChild = Node("source-child", "Node_Number", Json::array({Wire("source-input")}));
		sourceChild["group"] = "collection";
		auto localChild = Node("local-child", "Node_Number", Json::array({Wire("local-input")}));
		localChild["group"] = "local";
		auto nodes = Json::array(
			{outer, copy, baseCollection, localCollection, sourceInput, localInput, sourceChild, localChild}
		);
		if (nested) nodes.insert(nodes.begin(), Node("container", "Node_Group"));
		return {{"nodes", std::move(nodes)}};
	}
}

TEST_CASE(
	"PXC append Collection aliases follow source top-level and nested instance binding",
	"[imagegraphio][collection_alias_append]"
) {
	for (const bool nested : {false, true}) {
		DYNAMIC_SECTION("saved parent container=" << nested) {
			const auto source = Archive(Source(nested));
			PxcxAppendOptions options;
			options.Namespace = "placed";
			PxcxAppendResult result;
			engine::imagegraph::Diagnostic diagnostic;
			REQUIRE(AppendPxcxProject(source, source, options, result, diagnostic));
			const std::string expectedOwner = nested ? "placed/source-input" : "source-input";
			const auto alias = std::find_if(
				result.Project.Graph.Nodes.begin(), result.Project.Graph.Nodes.end(), [](const auto &node) {
					return node.Id == "placed/local-input";
				}
			);
			REQUIRE(alias != result.Project.Graph.Nodes.end());
			CHECK(alias->SourceParentInputBase == expectedOwner);
			CHECK(
				std::any_of(
					result.Project.GroupBindings.begin(),
					result.Project.GroupBindings.end(),
					[&](const auto &binding) {
						return binding.NodeId == "placed/local-input" && binding.OwnerId == expectedOwner &&
							   binding.Port == "parent_value";
					}
				)
			);
			const auto saved = Json::parse(
				result.Project.Source.GraphJson.begin(), result.Project.Source.GraphJson.end() - 1
			);
			const auto copy =
				std::find_if(saved.at("nodes").begin(), saved.at("nodes").end(), [](const auto &node) {
					return node.at("id") == "placed/copy";
				});
			REQUIRE(copy != saved.at("nodes").end());
			CHECK(copy->at("instanceBase") == (nested ? "placed/outer" : "outer"));
			REQUIRE(result.Project.GroupPrebinding.has_value());
			const auto prebindingAlias = std::find_if(
				result.Project.GroupPrebinding->Nodes.begin(),
				result.Project.GroupPrebinding->Nodes.end(),
				[](const auto &node) { return node.Id == "placed/local-input"; }
			);
			REQUIRE(prebindingAlias != result.Project.GroupPrebinding->Nodes.end());
			CHECK(prebindingAlias->SourceParentInputBase.empty());
		}
	}
}
