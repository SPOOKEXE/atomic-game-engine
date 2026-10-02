#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraphio.wav_watcher_property")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportWatcher(std::string_view attribute) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"({"nodes":[{"id":"wav","type":"Node_WAV_File_Read","x":0,"y":0,"inputs":[],"attri":{"future":7)";
		archive.GraphJson += attribute;
		archive.GraphJson += "}}]}";
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
TEST_CASE("WAV watcher source boolean survives native and PXC edits", "[imagegraphio][wav_watcher]") {
	auto imported = ImportWatcher(",\"file_checker\":false");
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.wav_file_read");
	REQUIRE(imported.Graph.Nodes[0].SourceProperties.size() == 1);
	CHECK(imported.Graph.Nodes[0].SourceProperties[0] == AuthoredValue{"file_checker", false});
	Document desired;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), desired, diagnostic) == Status::Ok);
	CHECK(desired == imported.Graph);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
	desired.Nodes[0].SourceProperties[0].Data = true;
	const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(archive, replay, failure));
	CHECK(replay.Graph.Nodes[0].SourceProperties == desired.Nodes[0].SourceProperties);
	CHECK(archive.GraphJson.find("\"future\":7") != std::string::npos);
	desired.Nodes[0].SourceProperties.clear();
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	REQUIRE(ImportPxcxImageGraph(archive, replay, failure));
	CHECK(replay.Graph.Nodes[0].SourceProperties.empty());
}
TEST_CASE(
	"Absent WAV watcher keeps the constructor default and nonbool mappings stay opaque",
	"[imagegraphio][wav_watcher]"
) {
	auto imported = ImportWatcher("");
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.wav_file_read");
	CHECK(imported.Graph.Nodes[0].SourceProperties.empty());
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto invalid = ImportWatcher(",\"file_checker\":0");
	CHECK(invalid.Graph.Nodes[0].Type != "pc.wav_file_read");
	auto invalidWanted = imported.Graph;
	invalidWanted.Nodes[0].SourceProperties.push_back({"file_checker", int64_t{0}});
	bytes = {std::byte{42}};
	CHECK_FALSE(WritePxcxProjection(imported, invalidWanted, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}
