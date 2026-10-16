#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/bake/Procedural.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.bake.procedural")
TEST_DEPENDS("engine.procedural.geometry")
TEST_DEPENDS("engine.assets.mesh")

using namespace engine;

namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;

	procedural::RegionGeometry Source() {
		procedural::RegionResult result;
		result.Placements = {
			{"tree/a", procedural::PlacementKind::Tree, "", {2 * CELL, 3 * CELL}, 0, 2, 1},
			{"lot/a", procedural::PlacementKind::VillageLot, "town", {4 * CELL, 3 * CELL}, 0, 0, 0},
			{"house/a", procedural::PlacementKind::House, "town", {5 * CELL, 3 * CELL}, 1, 1, 0},
			{"entrance/a", procedural::PlacementKind::Entrance, "gate", {6 * CELL, 3 * CELL}, 0, 0, 0}
		};
		result.Roads = {
			{"road/a", "gate", "town", {{{6 * CELL, 3 * CELL}, {8 * CELL, 4 * CELL}, {10 * CELL, 3 * CELL}}}}
		};
		procedural::RegionGeometry geometry;
		procedural::Diagnostic diagnostic;
		REQUIRE(
			procedural::BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) ==
			procedural::Status::Ok
		);
		return geometry;
	}

	const procedural::GeometryObject &Find(const procedural::RegionGeometry &geometry, std::string_view id) {
		const auto found =
			std::find_if(geometry.Objects.begin(), geometry.Objects.end(), [&](const auto &row) {
				return row.Id == id;
			});
		REQUIRE(found != geometry.Objects.end());
		return *found;
	}
}

TEST_CASE("procedural bake round-trips ordinary meshes and placement description", "[bake][procedural]") {
	const procedural::RegionGeometry source = Source();
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(bake::BakeProceduralGeometry(source, baked, failure));
	CHECK(failure.empty());
	REQUIRE(baked.Placements.size() == source.Objects.size());
	REQUIRE(baked.Meshes.size() == source.Objects.size());
	std::vector<bake::ProceduralPlacement> loaded;
	REQUIRE(bake::DecodeProceduralDescription(baked.DescriptionBytes, loaded, failure));
	REQUIRE(loaded.size() == source.Objects.size());
	for (const bake::ProceduralPlacement &placement : loaded) {
		const procedural::GeometryObject &expected = Find(source, placement.Id);
		CHECK(placement.Kind == expected.Kind);
		CHECK(placement.Origin == expected.Origin);
		CHECK(placement.Material == expected.Material);
		CHECK(placement.Variant == expected.Variant);
		CHECK(placement.Centre == expected.Centre);
		CHECK(placement.Size == expected.Size);
		CHECK(placement.Colour == expected.Colour);
		const auto asset = std::find_if(baked.Meshes.begin(), baked.Meshes.end(), [&](const auto &mesh) {
			return mesh.Name == placement.MeshName;
		});
		REQUIRE(asset != baked.Meshes.end());
		const std::string hash = assets::Hasher::Of(asset->Bytes).ToHex();
		CHECK(asset->Name == "procedural/" + hash + ".amesh");
		core::ByteReader reader(asset->Bytes);
		assets::MeshData ordinary;
		REQUIRE(assets::Mesh::Read(reader, ordinary));
		CHECK(reader.AtEnd());
		CHECK(ordinary.Indices == expected.Indices);
		REQUIRE(ordinary.Submeshes.size() == 1);
		for (float channel : ordinary.Submeshes.front().BaseColour)
			CHECK(channel == 1.0f);
		REQUIRE(ordinary.Vertices.size() == expected.Vertices.size());
		for (size_t index = 0; index < expected.Vertices.size(); ++index) {
			const auto &actual = ordinary.Vertices[index];
			const auto &vertex = expected.Vertices[index];
			for (size_t axis = 0; axis < 3; ++axis) {
				CHECK(actual.Position[axis] == vertex.Position[axis]);
				CHECK(actual.Normal[axis] == vertex.Normal[axis]);
			}
			for (size_t axis = 0; axis < 2; ++axis)
				CHECK(actual.TexCoord[axis] == vertex.UV[axis]);
		}
	}
}

TEST_CASE("equal local meshes share one content-addressed asset", "[bake][procedural]") {
	procedural::RegionResult result;
	result.Placements = {
		{"tree/a", procedural::PlacementKind::Tree, "", {CELL, CELL}, 0, 0, 1},
		{"tree/b", procedural::PlacementKind::Tree, "", {2 * CELL, CELL}, 0, 0, 1}
	};
	procedural::RegionGeometry geometry;
	procedural::Diagnostic diagnostic;
	REQUIRE(
		procedural::BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == procedural::Status::Ok
	);
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(bake::BakeProceduralGeometry(geometry, baked, failure));
	REQUIRE(baked.Meshes.size() == 1);
	REQUIRE(baked.Placements.size() == 2);
	CHECK(baked.Placements[0].MeshName == baked.Placements[1].MeshName);
	CHECK(baked.Placements[0].Origin != baked.Placements[1].Origin);
}

TEST_CASE("empty region has canonical description and no mesh assets", "[bake][procedural]") {
	procedural::RegionGeometry empty;
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(bake::BakeProceduralGeometry(empty, baked, failure));
	CHECK(baked.Meshes.empty());
	CHECK(baked.Placements.empty());
	CHECK(
		(baked.DescriptionBytes == std::vector<std::byte>{
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
	std::vector<bake::ProceduralPlacement> loaded;
	REQUIRE(bake::DecodeProceduralDescription(baked.DescriptionBytes, loaded, failure));
	CHECK(loaded.empty());
}

TEST_CASE("malformed description and duplicate identities preserve loaded placements", "[bake][procedural]") {
	procedural::RegionResult result;
	result.Placements = {
		{"tree/a", procedural::PlacementKind::Tree, "", {CELL, CELL}, 0, 0, 0},
		{"tree/b", procedural::PlacementKind::Tree, "", {2 * CELL, CELL}, 0, 0, 0}
	};
	procedural::RegionGeometry geometry;
	procedural::Diagnostic diagnostic;
	REQUIRE(
		procedural::BuildRegionGeometry(result, {0, 0, 16}, geometry, diagnostic) == procedural::Status::Ok
	);
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(bake::BakeProceduralGeometry(geometry, baked, failure));
	std::vector<bake::ProceduralPlacement> loaded;
	REQUIRE(bake::DecodeProceduralDescription(baked.DescriptionBytes, loaded, failure));
	REQUIRE(loaded.size() == 2);
	const auto savedId = loaded.front().Id;
	std::vector<std::byte> bytes = baked.DescriptionBytes;
	bytes.push_back(std::byte{0});
	CHECK_FALSE(bake::DecodeProceduralDescription(bytes, loaded, failure));
	CHECK(loaded.front().Id == savedId);
	bytes = baked.DescriptionBytes;
	const std::array<std::byte, 6> secondId{
		std::byte{'t'}, std::byte{'r'}, std::byte{'e'}, std::byte{'e'}, std::byte{'/'}, std::byte{'b'}
	};
	const auto found = std::search(bytes.begin(), bytes.end(), secondId.begin(), secondId.end());
	REQUIRE(found != bytes.end());
	*(found + 5) = std::byte{'a'};
	CHECK_FALSE(bake::DecodeProceduralDescription(bytes, loaded, failure));
	CHECK(loaded.size() == 2);
	CHECK(loaded.front().Id == savedId);
	bytes = baked.DescriptionBytes;
	const std::array<std::byte, 11> pathPrefix{
		std::byte{'p'},
		std::byte{'r'},
		std::byte{'o'},
		std::byte{'c'},
		std::byte{'e'},
		std::byte{'d'},
		std::byte{'u'},
		std::byte{'r'},
		std::byte{'a'},
		std::byte{'l'},
		std::byte{'/'}
	};
	const auto path = std::search(bytes.begin(), bytes.end(), pathPrefix.begin(), pathPrefix.end());
	REQUIRE(path != bytes.end());
	*path = std::byte{'.'};
	CHECK_FALSE(bake::DecodeProceduralDescription(bytes, loaded, failure));
	CHECK(loaded.size() == 2);
	bytes = baked.DescriptionBytes;
	const auto upperPath = std::search(bytes.begin(), bytes.end(), pathPrefix.begin(), pathPrefix.end());
	REQUIRE(upperPath != bytes.end());
	*(upperPath + pathPrefix.size()) = std::byte{'A'};
	CHECK_FALSE(bake::DecodeProceduralDescription(bytes, loaded, failure));
	CHECK(loaded.size() == 2);
	bytes = baked.DescriptionBytes;
	bytes[4] = std::byte{0xff};
	bytes[5] = std::byte{0xff};
	bytes[6] = std::byte{0xff};
	bytes[7] = std::byte{0xff};
	CHECK_FALSE(bake::DecodeProceduralDescription(bytes, loaded, failure));
	CHECK(loaded.size() == 2);
}

TEST_CASE("invalid geometry refuses the entire prior bake", "[bake][procedural]") {
	procedural::RegionGeometry geometry = Source();
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(bake::BakeProceduralGeometry(geometry, baked, failure));
	const auto savedDescription = baked.DescriptionBytes;
	const auto savedName = baked.Meshes.front().Name;
	geometry.Objects.front().Centre[0] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(bake::BakeProceduralGeometry(geometry, baked, failure));
	CHECK(baked.DescriptionBytes == savedDescription);
	CHECK(baked.Meshes.front().Name == savedName);
	geometry = Source();
	geometry.Objects.front().Centre[0] += 0.25f;
	CHECK_FALSE(bake::BakeProceduralGeometry(geometry, baked, failure));
	CHECK(baked.DescriptionBytes == savedDescription);
	geometry = Source();
	geometry.Objects.back().Id = geometry.Objects.front().Id;
	CHECK_FALSE(bake::BakeProceduralGeometry(geometry, baked, failure));
	CHECK(baked.DescriptionBytes == savedDescription);
	geometry = Source();
	geometry.VertexCount = procedural::GeometryLimits::HARD_MAXIMUM_VERTICES + 1;
	CHECK_FALSE(bake::BakeProceduralGeometry(geometry, baked, failure));
	CHECK(baked.DescriptionBytes == savedDescription);
}
