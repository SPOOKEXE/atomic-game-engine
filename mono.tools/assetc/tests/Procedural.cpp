#include <engine/assets/ContentForm.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <assetc/Bake.hpp>
#include <assetc/Procedural.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("tools.assetc.procedural")
TEST_DEPENDS("engine.assets.procedural_description")
TEST_DEPENDS("engine.procedural.artifact")
TEST_DEPENDS("engine.bake.procedural")

using namespace engine;

namespace {
	namespace fs = std::filesystem;
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr procedural::RegionScope SCOPE{-1, 0, 16};

	std::vector<uint8_t> Artifact() {
		core::ByteWriter key;
		key.WriteRaw("RGK2", 4);
		key.WriteInt32(SCOPE.X);
		key.WriteInt32(SCOPE.Y);
		key.WriteUInt16(SCOPE.CellsPerSide);
		const auto keyBytes = key.Bytes();
		procedural::RegionResult result;
		result.KeyBytes.reserve(keyBytes.size());
		for (std::byte byte : keyBytes)
			result.KeyBytes.push_back(static_cast<uint8_t>(byte));
		result.Placements.push_back(
			{"tree/a", procedural::PlacementKind::Tree, "", {-15 * CELL, 2 * CELL}, 0, 2, 1}
		);
		std::vector<uint8_t> bytes;
		procedural::Diagnostic diagnostic;
		REQUIRE(procedural::EncodeRegionResult(result, bytes, diagnostic) == procedural::Status::Ok);
		return bytes;
	}

	struct Scratch {
		fs::path Root;
		explicit Scratch(std::string_view label)
			: Root(fs::temp_directory_path() / ("assetc-procedural-" + std::string(label))) {
			std::error_code error;
			fs::remove_all(Root, error);
			fs::create_directories(Root / "raw", error);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;
	};

	void Write(const fs::path &path, std::span<const uint8_t> bytes) {
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		REQUIRE(file.good());
	}

	std::vector<std::byte> Read(const fs::path &path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		REQUIRE(file.good());
		const std::streamoff length = file.tellg();
		REQUIRE(length >= 0);
		std::vector<std::byte> bytes(static_cast<size_t>(length));
		file.seekg(0, std::ios::beg);
		file.read(reinterpret_cast<char *>(bytes.data()), length);
		REQUIRE(file.gcount() == length);
		return bytes;
	}
}

TEST_CASE(
	"accepted region cooks to ordinary meshes and a loadable PRD1 description", "[assetc][procedural]"
) {
	const auto artifact = Artifact();
	bake::ProceduralBake cooked;
	std::string failure;
	REQUIRE(assetc::CookProceduralArtifact(artifact, SCOPE, cooked, failure));
	REQUIRE(cooked.Meshes.size() == 1);
	REQUIRE(cooked.Placements.size() == 1);
	Scratch scratch("ordinary");
	REQUIRE(assetc::PublishProceduralBake(scratch.Root / "baked", "town/region.aproc", cooked, failure));
	CHECK(Read(scratch.Root / "baked" / cooked.Meshes.front().Name) == cooked.Meshes.front().Bytes);
	CHECK(Read(scratch.Root / "baked/town/region.aproc") == cooked.DescriptionBytes);

	std::vector<assets::ProceduralPlacement> loaded;
	REQUIRE(
		assets::DecodeProceduralDescription(Read(scratch.Root / "baked/town/region.aproc"), loaded, failure)
	);
	REQUIRE(loaded.size() == 1);
	CHECK(loaded.front().Id == "tree/a");
	CHECK(loaded.front().MeshName == cooked.Meshes.front().Name);
	CHECK(loaded.front().Origin.X == -15 * CELL);
	const auto meshBytes = Read(scratch.Root / "baked" / loaded.front().MeshName);
	core::ByteReader reader(meshBytes);
	assets::MeshData mesh;
	REQUIRE(assets::Mesh::Read(reader, mesh));
	CHECK(reader.AtEnd());
	CHECK_FALSE(mesh.Vertices.empty());
	CHECK_FALSE(mesh.Indices.empty());
	REQUIRE(assetc::PublishProceduralBake(scratch.Root / "baked", "town/region.aproc", cooked, failure));
}

TEST_CASE(
	"procedural publication refuses scope and conflicting immutable mesh without changing description",
	"[assetc][procedural]"
) {
	const auto artifact = Artifact();
	bake::ProceduralBake cooked;
	std::string failure;
	REQUIRE(assetc::CookProceduralArtifact(artifact, SCOPE, cooked, failure));
	bake::ProceduralBake previous = cooked;
	CHECK_FALSE(assetc::CookProceduralArtifact(artifact, {0, 0, 16}, previous, failure));
	CHECK(previous.DescriptionBytes == cooked.DescriptionBytes);
	Scratch scratch("refusal");
	const fs::path output = scratch.Root / "baked";
	REQUIRE(assetc::PublishProceduralBake(output, "region.aproc", cooked, failure));
	const auto lastGood = Read(output / "region.aproc");
	bake::ProceduralBake shifted = cooked;
	shifted.Placements.front().Centre[0] += 0.25f;
	REQUIRE(assets::EncodeProceduralDescription(shifted.Placements, shifted.DescriptionBytes, failure));
	CHECK_FALSE(assetc::PublishProceduralBake(output, "region.aproc", shifted, failure));
	CHECK(Read(output / "region.aproc") == lastGood);
	Write(output / cooked.Meshes.front().Name, std::span<const uint8_t>{});
	CHECK_FALSE(assetc::PublishProceduralBake(output, "region.aproc", cooked, failure));
	CHECK(Read(output / "region.aproc") == lastGood);
	CHECK_FALSE(assetc::PublishProceduralBake(output, "../escape.aproc", cooked, failure));
	CHECK_FALSE(fs::exists(scratch.Root / "escape.aproc"));
}

TEST_CASE("assetc dispatches bounded rga before its ordinary source reader", "[assetc][procedural]") {
	Scratch scratch("dispatch");
	const auto artifact = Artifact();
	Write(scratch.Root / "raw/region.rga", artifact);
	assetc::Settings settings;
	settings.Input = scratch.Root / "raw";
	settings.Output = scratch.Root / "baked";
	settings.Only = "region.rga";
	settings.CopyUnknown = false;
	std::string failure;
	const assetc::Report report = assetc::Bake(settings, failure);
	CHECK(failure.empty());
	CHECK(report.Failures == 0);
	REQUIRE(report.Assets.size() == 2);
	CHECK(assetc::BakedName("region.rga") == "region.aproc");
	CHECK(report.Assets.back().Output == "region.aproc");
	CHECK(report.Assets.back().Kind == assets::AssetKind::Data);
	CHECK(fs::exists(scratch.Root / "baked/region.aproc"));
	CHECK(fs::exists(scratch.Root / "baked" / report.Assets.front().Output));

	settings.Output.clear();
	const assetc::Report memory = assetc::Bake(settings, failure);
	REQUIRE(memory.Assets.size() == 2);
	CHECK(memory.Assets.front().Payload == Read(scratch.Root / "baked" / memory.Assets.front().Output));
	CHECK(memory.Assets.back().Payload == Read(scratch.Root / "baked/region.aproc"));
}

TEST_CASE("rga policy refusal precedes reading and a large source is bounded", "[assetc][procedural]") {
	Scratch scratch("limits");
	std::vector<uint8_t> oversized(procedural::ArtifactLimits{}.MaximumBytes + 1, 0x55);
	Write(scratch.Root / "raw/region.rga", oversized);
	assetc::Settings settings;
	settings.Input = scratch.Root / "raw";
	settings.Output = scratch.Root / "baked";
	settings.Only = "region.rga";
	settings.Content.Allow(assets::ContentForm::RegionArtifact, false);
	std::string failure;
	const assetc::Report refused = assetc::Bake(settings, failure);
	CHECK(refused.Refused == 1);
	CHECK(refused.Failures == 0);
	CHECK_FALSE(fs::exists(scratch.Root / "baked/region.aproc"));

	settings.Content.Allow(assets::ContentForm::RegionArtifact, true);
	const assetc::Report limited = assetc::Bake(settings, failure);
	CHECK(limited.Failures == 1);
	CHECK_FALSE(fs::exists(scratch.Root / "baked/region.aproc"));
	std::vector<uint8_t> loaded{7};
	CHECK_FALSE(assetc::ReadProceduralArtifactFile(scratch.Root / "raw", "../region.rga", loaded, failure));
	CHECK(loaded == std::vector<uint8_t>{7});
}
