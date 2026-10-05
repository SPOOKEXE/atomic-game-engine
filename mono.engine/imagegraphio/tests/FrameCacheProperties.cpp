#include <engine/imagegraph/CacheGroupReplay.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraphio.frame_cache_properties")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport Imported(std::string_view type, std::string_view attributes, std::string_view data = "") {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = "{\"nodes\":[{\"id\":\"cache\",\"type\":\"" + std::string(type) +
							"\",\"x\":0,\"y\":0,\"inputs\":[],\"attri\":{\"future\":7" +
							std::string(attributes) + "}" + std::string(data) + "}]}";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
}
TEST_CASE(
	"Source cache serialized attributes and cache text survive typed native and PXC editing",
	"[imagegraphio][source_frame_cache]"
) {
	for (const auto type : {"Node_Cache", "Node_Cache_Array"}) {
		auto imported =
			Imported(type, ",\"serialize\":false,\"cache_group\":[\"producer\"]", ",\"cache\":\"[]\"");
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(
			imported.Graph.Nodes[0].Type ==
			(std::string(type) == "Node_Cache" ? "pc.cache" : "pc.cache_array")
		);
		REQUIRE(imported.Graph.Nodes[0].SourceProperties.size() == 3);
		Document desired;
		Diagnostic e;
		REQUIRE(Read(Write(imported.Graph), desired, e) == Status::Ok);
		CHECK(desired == imported.Graph);
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, e));
		CHECK(bytes == imported.Source.OriginalBytes);
		desired.Nodes[0].SourceProperties[0].Data = true;
		desired.Nodes[0].SourceProperties[1].Data =
			ArrayValue{ValueType::Text, {std::string("next"), std::string("other")}};
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, e));
		engine::bake::PxcxArchive archive;
		std::string error;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, error));
		PxcxImport replay;
		REQUIRE(ImportPxcxImageGraph(archive, replay, error));
		CHECK(replay.Graph.Nodes[0].SourceProperties == desired.Nodes[0].SourceProperties);
		CHECK(archive.GraphJson.find("\"future\":7") != std::string::npos);
		desired.Nodes[0].SourceProperties.clear();
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, e));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, error));
		REQUIRE(ImportPxcxImageGraph(archive, replay, error));
		CHECK(replay.Graph.Nodes[0].SourceProperties.empty());
		CHECK(archive.GraphJson.find("\"cache\":") == std::string::npos);
	}
}
TEST_CASE(
	"Absent source cache metadata remains absent and malformed source metadata stays opaque",
	"[imagegraphio][source_frame_cache]"
) {
	auto base = Imported("Node_Cache", "");
	REQUIRE(base.Graph.Nodes[0].Type == "pc.cache");
	CHECK(base.Graph.Nodes[0].SourceProperties.empty());
	for (const auto malformed : {",\"serialize\":1", ",\"cache_group\":[5]", ",\"cache_group\":false"}) {
		auto opaque = Imported("Node_Cache", malformed);
		CHECK(opaque.Graph.Nodes[0].Type != "pc.cache");
	}
	auto saved = Imported("Node_Cache_Array", "", ",\"cache\":false");
	CHECK(saved.Graph.Nodes[0].Type != "pc.cache_array");
	auto desired = base.Graph;
	desired.Nodes[0].SourceProperties = {{"serialize", int64_t{1}}};
	std::vector<std::byte> bytes{std::byte{42}};
	Diagnostic e;
	CHECK_FALSE(WritePxcxProjection(base, desired, {}, bytes, e));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}

TEST_CASE(
	"Membership transfer writes both source owner lists while preserving opaque attributes",
	"[imagegraphio][pxcx][cache_group][membership]"
) {
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson =
		R"({"nodes":[{"id":"cache-a","type":"Node_Cache","x":0,"y":0,"inputs":[],"attri":{"serialize":true,"cache_group":["number","missing","number"],"future":7}},{"id":"cache-b","type":"Node_Cache_Array","x":1,"y":0,"inputs":[],"attri":{"serialize":false,"cache_group":[],"future":8}},{"id":"number","type":"Node_Number_Simple","x":2,"y":0,"inputs":[{"r":{"d":99}}]}],"future":{"project":9}})";
	archive.GraphJson.push_back('\0');
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	engine::bake::PxcxArchive checked;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
	auto desired = imported.Graph;
	REQUIRE(desired.Nodes.size() == 3);
	REQUIRE(desired.Nodes[0].Id == "cache-a");
	REQUIRE(desired.Nodes[1].Id == "cache-b");
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(desired, {}, state, Limits::MaximumEvaluationBytes, error) ==
		Status::Ok
	);
	auto checkpoint = state;
	const std::array journals{&state, &checkpoint};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(
			desired, journals, "cache-b", "number", Limits::MaximumEvaluationBytes, error
		) == Status::Ok
	);
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, error));
	engine::bake::PxcxArchive saved;
	REQUIRE(engine::bake::ReadPxcx(bytes, saved, failure));
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(saved, replay, failure));
	CHECK(replay.Graph.Nodes[0].SourceProperties == desired.Nodes[0].SourceProperties);
	CHECK(replay.Graph.Nodes[1].SourceProperties == desired.Nodes[1].SourceProperties);
	const auto group = [](const Node &node) -> const ArrayValue & {
		const auto found = std::find_if(
			node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &property) {
				return property.Port == "cache_group";
			}
		);
		REQUIRE(found != node.SourceProperties.end());
		return std::get<ArrayValue>(found->Data);
	};
	CHECK(group(replay.Graph.Nodes[0]).Elements == std::vector<ElementValue>{std::string{"missing"}});
	CHECK(group(replay.Graph.Nodes[1]).Elements == std::vector<ElementValue>{std::string{"number"}});
	CHECK(saved.GraphJson.find("\"future\":7") != std::string::npos);
	CHECK(saved.GraphJson.find("\"future\":8") != std::string::npos);
	CHECK(saved.GraphJson.find("\"project\":9") != std::string::npos);
}
