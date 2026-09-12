// Deterministic automatic LOD generation from valid mesh assets.

#include <engine/assets/MeshDecimate.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.assets.mesh-decimate")
TEST_DEPENDS("engine.assets.mesh")

namespace {
	engine::assets::MeshVertex At(float x, float y, float z) {
		engine::assets::MeshVertex vertex{};
		vertex.Position[0] = x;
		vertex.Position[1] = y;
		vertex.Position[2] = z;
		vertex.Normal[1] = 1.0f;
		vertex.TexCoord[0] = x;
		vertex.TexCoord[1] = z;
		return vertex;
	}

	engine::assets::MeshData QuadPair() {
		using engine::assets::Submesh;
		engine::assets::MeshData data;
		data.Vertices = {
			At(0.0f, 0.0f, 0.0f),
			At(1.0f, 0.0f, 0.0f),
			At(1.0f, 0.0f, 1.0f),
			At(0.0f, 0.0f, 1.0f),
		};
		data.Indices = {0, 1, 2, 0, 2, 3};
		data.Submeshes.push_back(Submesh{0, 6, "floor", "textures/floor"});
		data.ComputeBounds();
		return data;
	}
}

TEST_CASE("mesh decimation is deterministic and keeps a valid material run", "[assets][mesh-decimate]") {
	using namespace engine::assets;
	const MeshData source = QuadPair();
	MeshData first;
	MeshData second;
	REQUIRE(DecimateMesh(source, 0.5f, first));
	REQUIRE(DecimateMesh(source, 0.5f, second));

	REQUIRE(first.IsValid());
	CHECK(first.Indices.size() == 3);
	REQUIRE(first.Submeshes.size() == 1);
	CHECK(first.Submeshes[0].FirstIndex == 0);
	CHECK(first.Submeshes[0].IndexCount == first.Indices.size());
	CHECK(first.Submeshes[0].Material == source.Submeshes[0].Material);
	CHECK(first.Submeshes[0].Texture == source.Submeshes[0].Texture);
	CHECK(first.Minimum.Y == 0.0f);
	CHECK(first.Maximum.Y == 0.0f);
	CHECK(first.Indices == second.Indices);
	CHECK(first.Vertices.size() == second.Vertices.size());

	SECTION("skinning is never blended across different influences") {
		MeshData skinned = source;
		skinned.JointCount = 2;
		for (size_t index = 0; index < skinned.Vertices.size(); index++) {
			skinned.Vertices[index].Joints[0] = static_cast<uint16_t>(index % 2);
			skinned.Vertices[index].Weights[0] = 65535;
		}
		MeshData reduced;
		REQUIRE(DecimateMesh(skinned, 0.5f, reduced));
		for (const MeshVertex &vertex : reduced.Vertices) {
			CHECK(vertex.Weights[0] == 65535);
			CHECK(vertex.Joints[0] < reduced.JointCount);
		}
	}
}

TEST_CASE("an automatic mesh ladder publishes independent valid mesh data", "[assets][mesh-decimate]") {
	using namespace engine::assets;
	const MeshData source = QuadPair();
	std::array<MeshData, 3> ladder;
	const std::array ratios{0.5f, 0.25f, 0.125f};

	REQUIRE(BuildMeshLodLadder(source, ratios, ladder));
	for (const MeshData &level : ladder) {
		CHECK(level.IsValid());
		CHECK(level.Indices.size() == 3);
		CHECK(level.Submeshes.size() == source.Submeshes.size());
		CHECK(level.Submeshes[0].Material == source.Submeshes[0].Material);
		CHECK(level.Submeshes[0].Texture == source.Submeshes[0].Texture);
	}

	std::array<MeshData, 2> wrongSize;
	CHECK_FALSE(BuildMeshLodLadder(source, ratios, wrongSize));
}

TEST_CASE("mesh decimation preserves partial material runs and uncovered faces", "[assets][mesh-decimate]") {
	using namespace engine::assets;
	MeshData source = QuadPair();
	source.Submeshes[0].IndexCount = 3;

	MeshData reduced;
	REQUIRE(DecimateMesh(source, 0.5f, reduced));
	REQUIRE(reduced.IsValid());
	REQUIRE(reduced.Submeshes.size() == 1);
	CHECK(reduced.Submeshes[0].FirstIndex == 0);
	CHECK(reduced.Submeshes[0].IndexCount == 3);
	CHECK(reduced.Submeshes[0].Material == "floor");
	CHECK(reduced.Indices.size() == 6);
}

TEST_CASE("mesh decimation fallback retains the largest isolated face", "[assets][mesh-decimate]") {
	using namespace engine::assets;
	MeshData source;
	source.Vertices = {
		At(0.0f, 0.0f, 0.0f),
		At(0.1f, 0.0f, 0.0f),
		At(0.0f, 0.0f, 0.1f),
		At(2.0f, 0.0f, 0.0f),
		At(6.0f, 0.0f, 0.0f),
		At(2.0f, 0.0f, 4.0f),
	};
	source.Indices = {0, 1, 2, 3, 4, 5};
	source.ComputeBounds();

	MeshData reduced;
	REQUIRE(DecimateMesh(source, 0.5f, reduced));
	REQUIRE(reduced.IsValid());
	REQUIRE(reduced.Vertices.size() == 3);
	CHECK(reduced.Indices == std::vector<uint32_t>{0, 1, 2});
	CHECK(reduced.Vertices[0].Position[0] == 2.0f);
	CHECK(reduced.Vertices[1].TexCoord[0] == 6.0f);
	CHECK(reduced.Vertices[2].Normal[1] == 1.0f);
}

TEST_CASE(
	"surface reduction retains the face with the largest expected projected area", "[assets][mesh-decimate]"
) {
	using namespace engine::assets;
	MeshData source;
	source.Vertices = {
		At(0.0f, 0.0f, 0.0f),
		At(0.1f, 0.0f, 0.0f),
		At(0.0f, 0.0f, 0.1f),
		At(2.0f, 0.0f, 0.0f),
		At(6.0f, 0.0f, 0.0f),
		At(2.0f, 0.0f, 4.0f),
	};
	source.Indices = {0, 1, 2, 3, 4, 5};
	source.ComputeBounds();

	MeshData reduced;
	REQUIRE(ReduceMesh(source, 0.5f, reduced));
	REQUIRE(reduced.IsValid());
	CHECK(reduced.Indices == std::vector<uint32_t>{0, 1, 2});
	CHECK(reduced.Vertices[0].Position[0] == 2.0f);

	std::array<MeshData, 1> ladder;
	REQUIRE(BuildReducedMeshLodLadder(source, std::array{0.5f}, ladder));
	CHECK(ladder[0].Indices == reduced.Indices);
}
