#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <studio/PxcxSave.hpp>

TEST_SUITE_ID("studio.pxcxsave")
TEST_DEPENDS("engine.imagegraphio.pxcxstructureedit")

namespace {
	struct Directory {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("atomic-pxc-save-test-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Directory() {
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Directory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};
	engine::imagegraphio::PxcxImport Source() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":3}}],"future":{"keep":9}},{"id":"opaque","type":"future.node","x":0,"y":0,"inputs":[]}],"future":{"project":8}})";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		engine::imagegraphio::PxcxImport result;
		REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, result, failure));
		return result;
	}
	std::vector<std::byte> Read(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		REQUIRE(stream.is_open());
		const auto count = stream.tellg();
		REQUIRE(count > 0);
		std::vector<std::byte> bytes(static_cast<size_t>(count));
		stream.seekg(0);
		stream.read(reinterpret_cast<char *>(bytes.data()), count);
		REQUIRE(stream.gcount() == count);
		return bytes;
	}
}

TEST_CASE(
	"Studio PXC save retains opaque metadata and reloads the edited native projection", "[studio][pxcx_save]"
) {
	const auto source = Source();
	auto authored = source.Graph;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Migrate(authored, diagnostic) == engine::imagegraph::Status::Ok);
	authored.Nodes.front().Position = {30, -20};
	Directory directory;
	const auto path = directory.Path / "modified.pxc";
	REQUIRE(studio::SavePxcxProjection(path, source.Source, authored, {}, diagnostic));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(Read(path), archive, failure));
	CHECK(archive.GraphJson.find(R"("future":{"keep":9})") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	REQUIRE(engine::imagegraph::Migrate(reloaded.Graph, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(reloaded.Graph == authored);
	REQUIRE(studio::SavePxcxProjection(path, source.Source, source.Graph, {}, diagnostic));
	CHECK(Read(path) == source.Source.OriginalBytes);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
		) == 1
	);
}

TEST_CASE(
	"Studio PXC inverse failures preserve the destination and leave no temporary file", "[studio][pxcx_save]"
) {
	const auto source = Source();
	Directory directory;
	const auto path = directory.Path / "existing.pxc";
	{
		std::ofstream stream(path, std::ios::binary);
		stream << "keep this destination";
	}
	const auto original = Read(path);
	auto authored = source.Graph;
	authored.Nodes.front().InstanceBase = "unmapped-new-instance";
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(studio::SavePxcxProjection(path, source.Source, authored, {}, diagnostic));
	CHECK(Read(path) == original);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
		) == 1
	);
	CHECK_FALSE(studio::SavePxcxProjection({}, source.Source, source.Graph, {}, diagnostic));
}
