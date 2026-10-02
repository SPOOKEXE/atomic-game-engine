#include "../src/TileProperties.hpp"

#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraphio.tile_properties")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportTiles() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"({"nodes":[{"id":"tiles","type":"Node_Tile_Tileset","x":0,"y":0,"inputs":[],"attri":{"animatedTiles":[{"name":"step","index":[0,1],"size":2,"unknown":{"keep":7}}],"autoterrain":[{"name":"ground","index":[0,1],"type":4}],"ruleTiles":[]}}]})";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
}
TEST_CASE(
	"Tile PXC records project to authored field pairs and retain exact no-op bytes",
	"[imagegraphio][tile_properties]"
) {
	auto imported = ImportTiles();
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.tile_tileset");
	REQUIRE(imported.Graph.Nodes[0].SourceProperties.size() == 3);
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	std::vector<std::byte> bytes;
	const bool written = WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	CHECK(bytes == imported.Source.OriginalBytes);
	auto &property = restored.Nodes[0].SourceProperties[0].Data;
	nlohmann::json records;
	REQUIRE(
		engine::imagegraphio::detail::EncodeTileProperty(property, records, Limits::MaximumEvaluationBytes)
	);
	records[0]["name"] = "edited";
	engine::imagegraphio::detail::ImportBudget budget(Limits::MaximumEvaluationBytes);
	Value changed;
	REQUIRE(engine::imagegraphio::detail::DecodeTileProperty(records, changed, &budget));
	property = std::move(changed);
	REQUIRE(WritePxcxProjection(imported, restored, {}, bytes, diagnostic));
	engine::bake::PxcxArchive saved;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, saved, failure));
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(saved, replay, failure));
	CHECK(replay.Graph.Nodes[0].SourceProperties == restored.Nodes[0].SourceProperties);
	CHECK(saved.GraphJson.find("unknown") != std::string::npos);
	CHECK(saved.GraphJson.find("edited") != std::string::npos);
}
TEST_CASE(
	"Tile keyed color map inverse retains unrelated source fields and refuses budget overflow",
	"[imagegraphio][tile_properties]"
) {
	const nlohmann::json source = {
		{"255", {{"color", 255}, {"target", 0}, {"future", nullptr}}},
		{"65280", {{"color", 65280}, {"target", {0, 1}}}}
	};
	engine::imagegraphio::detail::ImportBudget budget(Limits::MaximumEvaluationBytes);
	Value value;
	REQUIRE(engine::imagegraphio::detail::DecodeTileColorMap(source, value, &budget));
	nlohmann::json encoded;
	REQUIRE(
		engine::imagegraphio::detail::EncodeTileColorMap(
			value, source, encoded, Limits::MaximumEvaluationBytes
		)
	);
	CHECK(encoded == source);
	const Value sentinel = value;
	engine::imagegraphio::detail::ImportBudget tiny(1);
	CHECK_FALSE(engine::imagegraphio::detail::DecodeTileColorMap(source, value, &tiny));
	CHECK(value == sentinel);
	nlohmann::json output = "sentinel";
	CHECK_FALSE(engine::imagegraphio::detail::EncodeTileColorMap(value, source, output, 1));
	CHECK(output == "sentinel");
}
