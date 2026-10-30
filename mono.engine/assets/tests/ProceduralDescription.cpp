#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.assets.procedural_description")

using namespace engine;

namespace {
	assets::ProceduralPlacement Placement(std::string id) {
		assets::ProceduralPlacement placement;
		placement.Id = std::move(id);
		placement.Kind = "tree";
		placement.MeshName = "procedural/" + std::string(64, 'a') + ".amesh";
		placement.Origin = {-1024, 2048};
		placement.Material = 2;
		placement.Variant = 3;
		placement.Centre = {0.5f, 1.0f, -0.5f};
		placement.Size = {1.0f, 2.0f, 1.0f};
		placement.Colour = {0.25f, 0.5f, 0.75f, 1.0f};
		return placement;
	}
}

TEST_CASE("empty procedural description has fixed canonical bytes", "[assets][procedural]") {
	std::vector<std::byte> encoded;
	std::string failure;
	REQUIRE(assets::EncodeProceduralDescription({}, encoded, failure));
	CHECK(
		(encoded == std::vector<std::byte>{
						std::byte{'P'},
						std::byte{'R'},
						std::byte{'D'},
						std::byte{'1'},
						std::byte{0},
						std::byte{0},
						std::byte{0},
						std::byte{0}
					})
	);
	std::vector<assets::ProceduralPlacement> decoded{Placement("previous")};
	REQUIRE(assets::DecodeProceduralDescription(encoded, decoded, failure));
	CHECK(decoded.empty());
}

TEST_CASE("procedural placement round-trips exact ordinary asset metadata", "[assets][procedural]") {
	const std::array placements{Placement("tree/a"), Placement("tree/b")};
	std::vector<std::byte> encoded;
	std::string failure;
	REQUIRE(assets::EncodeProceduralDescription(placements, encoded, failure));
	std::vector<assets::ProceduralPlacement> decoded;
	REQUIRE(assets::DecodeProceduralDescription(encoded, decoded, failure));
	REQUIRE(decoded.size() == placements.size());
	for (size_t index = 0; index < placements.size(); ++index) {
		CHECK(decoded[index].Id == placements[index].Id);
		CHECK(decoded[index].Kind == placements[index].Kind);
		CHECK(decoded[index].MeshName == placements[index].MeshName);
		CHECK(decoded[index].Origin == placements[index].Origin);
		CHECK(decoded[index].Material == placements[index].Material);
		CHECK(decoded[index].Variant == placements[index].Variant);
		CHECK(decoded[index].Centre == placements[index].Centre);
		CHECK(decoded[index].Size == placements[index].Size);
		CHECK(decoded[index].Colour == placements[index].Colour);
	}
	std::vector<std::byte> again;
	REQUIRE(assets::EncodeProceduralDescription(decoded, again, failure));
	CHECK(again == encoded);
}

TEST_CASE(
	"PRD1 rejects malformed and noncanonical input without replacing last good", "[assets][procedural]"
) {
	const auto valid = Placement("tree/a");
	std::vector<std::byte> encoded;
	std::string failure;
	REQUIRE(assets::EncodeProceduralDescription(std::span(&valid, 1), encoded, failure));
	std::vector<assets::ProceduralPlacement> loaded{Placement("last-good")};
	const auto unchanged = loaded.front().Id;

	std::vector<std::byte> malformed = encoded;
	malformed[0] = std::byte{'X'};
	CHECK_FALSE(assets::DecodeProceduralDescription(malformed, loaded, failure));
	CHECK(loaded.front().Id == unchanged);
	malformed = encoded;
	malformed.push_back(std::byte{0});
	CHECK_FALSE(assets::DecodeProceduralDescription(malformed, loaded, failure));
	CHECK(loaded.front().Id == unchanged);

	// A finite signed zero is numerically valid but has only one canonical spelling.
	malformed = encoded;
	const size_t centreOffset = 8 + 12 + valid.Id.size() + valid.Kind.size() + valid.MeshName.size() + 16 + 4;
	REQUIRE(centreOffset + 4 <= malformed.size());
	malformed[centreOffset] = std::byte{0};
	malformed[centreOffset + 1] = std::byte{0};
	malformed[centreOffset + 2] = std::byte{0};
	malformed[centreOffset + 3] = std::byte{0x80};
	CHECK_FALSE(assets::DecodeProceduralDescription(malformed, loaded, failure));
	CHECK(loaded.front().Id == unchanged);
}

TEST_CASE("PRD1 encoder refuses duplicate IDs and nonfinite or forged metadata", "[assets][procedural]") {
	std::vector<std::byte> encoded{std::byte{0x42}};
	std::string failure;
	const auto duplicate = Placement("tree/a");
	const std::array duplicates{duplicate, duplicate};
	CHECK_FALSE(assets::EncodeProceduralDescription(duplicates, encoded, failure));
	CHECK(encoded == std::vector<std::byte>{std::byte{0x42}});

	auto invalid = duplicate;
	invalid.Centre[0] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(assets::EncodeProceduralDescription(std::span(&invalid, 1), encoded, failure));
	invalid = duplicate;
	invalid.MeshName[11] = 'A';
	CHECK_FALSE(assets::EncodeProceduralDescription(std::span(&invalid, 1), encoded, failure));
	invalid = duplicate;
	invalid.Kind = "unknown";
	CHECK_FALSE(assets::EncodeProceduralDescription(std::span(&invalid, 1), encoded, failure));
	CHECK(encoded == std::vector<std::byte>{std::byte{0x42}});
}

TEST_CASE("procedural mesh names use canonical lowercase hash spelling", "[assets][procedural]") {
	auto placement = Placement("tree/a");
	std::vector<std::byte> encoded;
	std::string failure;
	for (const std::string &digest : {std::string(64, '0'), std::string(64, 'f')}) {
		placement.MeshName = "procedural/" + digest + ".amesh";
		REQUIRE(assets::EncodeProceduralDescription(std::span(&placement, 1), encoded, failure));
		std::vector<assets::ProceduralPlacement> decoded;
		REQUIRE(assets::DecodeProceduralDescription(encoded, decoded, failure));
		REQUIRE(decoded.size() == 1);
		CHECK(decoded.front().MeshName == placement.MeshName);
	}
	for (const std::string &digest :
		 {std::string(63, 'a'),
		  std::string(65, 'a'),
		  std::string(63, 'a') + "A",
		  std::string(63, 'a') + "g"}) {
		placement.MeshName = "procedural/" + digest + ".amesh";
		CHECK_FALSE(assets::EncodeProceduralDescription(std::span(&placement, 1), encoded, failure));
	}
}

TEST_CASE("placement frames and material runs agree with their ordinary mesh", "[assets][procedural]") {
	auto placement = Placement("tree/a");
	placement.Centre = {0.5f, 0.0f, 0.5f};
	placement.Size = {1.0f, 0.0f, 1.0f};
	assets::MeshData mesh;
	mesh.Vertices.resize(3);
	mesh.Vertices[0].Position[0] = 0.0f;
	mesh.Vertices[0].Position[2] = 0.0f;
	mesh.Vertices[1].Position[0] = 1.0f;
	mesh.Vertices[1].Position[2] = 0.0f;
	mesh.Vertices[2].Position[0] = 0.0f;
	mesh.Vertices[2].Position[2] = 1.0f;
	mesh.Indices = {0, 1, 2};
	assets::Submesh run;
	run.IndexCount = 3;
	run.Material = "procedural/tree/2";
	mesh.Submeshes.push_back(run);
	std::string failure;
	REQUIRE(assets::ValidateProceduralPlacementMesh(placement, mesh, failure));
	placement.Centre[0] = 0.6f;
	CHECK_FALSE(assets::ValidateProceduralPlacementMesh(placement, mesh, failure));
	placement.Centre[0] = 0.5f;
	mesh.Submeshes.front().BaseColour[0] = 0.5f;
	CHECK_FALSE(assets::ValidateProceduralPlacementMesh(placement, mesh, failure));
	mesh.Submeshes.front().BaseColour[0] = 1.0f;
	mesh.Vertices.front().Joints[0] = 1;
	CHECK_FALSE(assets::ValidateProceduralPlacementMesh(placement, mesh, failure));
}
