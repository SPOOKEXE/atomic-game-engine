#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcxreservedroutes")

using namespace engine::imagegraphio;
using namespace engine::imagegraph;

namespace {
	using Json = nlohmann::ordered_json;
	PxcxImport Import(Json root) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = root.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		const bool accepted = ImportPxcxImageGraph(archive, result, failure);
		INFO(failure);
		REQUIRE(accepted);
		return result;
	}
	Json Source(int64_t tag, int64_t index, bool output = false) {
		return Json{
			{"nodes",
			 Json::array(
				 {Json{
					  {"id", "producer"},
					  {"type", "Node_Number_Simple"},
					  {"x", 1},
					  {"y", 2},
					  {"inputs", Json::array({Json{{"r", {{"d", 3}}}}})}
				  },
				  Json{
					  {"id", "consumer"},
					  {"type", output ? "Node_Project_Output" : "Node_Number_Simple"},
					  {"x", 4},
					  {"y", 5},
					  {"inputs",
					   Json::array({Json{
						   {"r", {{"d", 7}}},
						   {"from_node", "producer"},
						   {"from_index", index},
						   {"from_tag", tag},
						   {"future", {{"keep", 19}}}
					   }})}
				  }}
			 )},
			{"future", {{"project", 23}}}
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
}

TEST_CASE(
	"Unsupported common source selectors stay opaque and survive position saves",
	"[imagegraphio][pxcx_projection]"
) {
	struct Selector {
		int64_t Tag;
		int64_t Index;
		const char *Port;
	};
	const std::array selectors{
		Selector{-2, -1, "pxcx.update_in_trigger"},
		Selector{-3, -1, "pxcx.updated_out_trigger"},
		Selector{-4, 0, "pxcx.metadata.0"},
		Selector{-4, 1, "pxcx.metadata.1"},
		Selector{-2, 1008, "pxcx.update_in_trigger"}
	};
	for (const auto &selector : selectors) {
		CAPTURE(selector.Tag, selector.Index);
		auto raw = Source(selector.Tag, selector.Index);
		raw["nodes"][0]["type"] = "Unknown_Future_Node";
		const auto imported = Import(raw);
		REQUIRE(imported.Graph.Nodes.size() == 2);
		for (const auto &node : imported.Graph.Nodes)
			CHECK(node.Type.starts_with("pxcx.opaque/"));
		REQUIRE(imported.Graph.Links.size() == 1);
		CHECK(imported.Graph.Links.front().FromPort == selector.Port);
		CHECK(imported.Graph.Links.front().ToPort == "input-0");
		Diagnostic diagnostic;
		auto desired = imported.Graph;
		REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		CHECK(bytes == imported.Source.OriginalBytes);
		desired.Nodes.front().Position = {31, 42};
		const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		const auto reopened = Reopen(bytes);
		CHECK(reopened.Graph.Links == imported.Graph.Links);
		auto expected = raw;
		expected["nodes"][0]["x"] = 31;
		expected["nodes"][0]["y"] = 42;
		CHECK(
			Json::parse(reopened.Source.GraphJson.begin(), reopened.Source.GraphJson.end() - 1) == expected
		);
	}
}

TEST_CASE(
	"Common source output cannot become an ordinary project image output", "[imagegraphio][pxcx_projection]"
) {
	auto raw = Source(-3, -1, true);
	raw["nodes"][0]["type"] = "Unknown_Future_Node";
	const auto imported = Import(raw);
	REQUIRE(imported.Graph.Outputs.size() == 1);
	CHECK(imported.Graph.Outputs.front().Port == "pxcx.updated_out_trigger");
	bool diagnosed = false;
	for (const auto &diagnostic : imported.Diagnostics)
		if (diagnostic.NodeId == "consumer" &&
			diagnostic.Message ==
				"PXCX output uses a common source socket whose lifecycle is not represented")
			diagnosed = true;
	CHECK(diagnosed);
}

TEST_CASE("Unsupported common source route edits refuse atomically", "[imagegraphio][pxcx_projection]") {
	auto raw = Source(-2, -1);
	raw["nodes"][0]["type"] = "Unknown_Future_Node";
	const auto imported = Import(raw);
	auto desired = imported.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Links.front().FromPort = "pxcx.updated_out_trigger";
	std::vector<std::byte> bytes{std::byte{0x42}};
	const std::vector<std::byte> previous{std::byte{0x42}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC common route has no attested native source callback");
	CHECK(bytes == previous);
}

TEST_CASE("Unreserved source tags retain ordinary positional routing", "[imagegraphio][pxcx_projection]") {
	const auto imported = Import(Source(12, 0));
	REQUIRE(imported.Graph.Links.size() == 1);
	CHECK(imported.Graph.Links.front().FromPort == "number");
	CHECK(imported.Graph.Links.front().ToPort == "value");
	for (const auto &node : imported.Graph.Nodes)
		CHECK_FALSE(node.Type.starts_with("pxcx.opaque/"));
}

TEST_CASE(
	"Common Update destinations stay distinct from ordinary input two on save and reopen",
	"[imagegraphio][pxcx_projection]"
) {
	auto raw = Source(12, 0);
	auto &consumer = raw["nodes"][1];
	consumer["inputs"] = Json::array(
		{Json{{"r", {{"d", 7}}}},
		 Json::object(),
		 Json{{"from_node", "producer"}, {"from_index", 0}, {"from_tag", 12}}}
	);
	consumer["inspectInputs"] = Json::array(
		{Json::object(),
		 Json::object(),
		 Json{
			 {"r", {{"d", false}}},
			 {"from_node", "producer"},
			 {"from_index", 0},
			 {"from_tag", 12},
			 {"future", {{"keep", 29}}}
		 }}
	);
	const auto imported = Import(raw);
	REQUIRE(imported.Graph.Links.size() == 2);
	CHECK(imported.Graph.Links.front().ToPort == "input-2");
	CHECK(imported.Graph.Links.back().ToPort == "pxcx.update_in_trigger");
	for (const auto &link : imported.Graph.Links)
		CHECK(link.FromPort == "output-0");
	for (const auto &node : imported.Graph.Nodes)
		CHECK(node.Type.starts_with("pxcx.opaque/"));
	auto desired = imported.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	desired.Nodes.front().Position = {31, 42};
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	const auto reopened = Reopen(bytes);
	CHECK(reopened.Graph.Links == imported.Graph.Links);
	auto expected = raw;
	expected["nodes"][0]["x"] = 31;
	expected["nodes"][0]["y"] = 42;
	CHECK(Json::parse(reopened.Source.GraphJson.begin(), reopened.Source.GraphJson.end() - 1) == expected);
	const auto previous = bytes;
	desired.Links.clear();
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC common route has no attested native source callback");
	CHECK(bytes == previous);
}
