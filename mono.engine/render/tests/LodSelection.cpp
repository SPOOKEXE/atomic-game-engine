// Authored mesh LOD host layout and mesh-bound fitting.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/vec4.hpp>

#include <LodSelection.hpp>
#include <array>
#include <cstdint>
#include <limits>

TEST_SUITE_ID("engine.render.lodselection")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::render::AppendAuthoredLod;
using engine::render::LodPlan;
using engine::render::MeshEntry;
using engine::render::MeshRange;
using engine::render::ModelMatrixOf;
using engine::render::TransferLayoutOf;
using engine::scene::DrawInstance;
using engine::scene::LodStrategy;

namespace {
	MeshEntry Mesh(Vector3 centre, Vector3 extent, uint32_t triangles, int32_t vertexOffset) {
		MeshEntry mesh;
		mesh.Centre = centre;
		mesh.Extent = extent;
		mesh.Whole = MeshRange{0, triangles * 3, vertexOffset};
		return mesh;
	}
}

TEST_CASE("an authored ladder packs every mesh against the same part bounds", "[render][lod]") {
	DrawInstance instance;
	instance.Frame = CFrame(Vector3(8.0f, -3.0f, 20.0f));
	instance.HalfExtent = Vector3(2.0f, 3.0f, 4.0f);
	instance.LodStrategyMode = LodStrategy::Authored;
	instance.LodLevels = 4;
	instance.LodTargetQuadArea = 9.0f;
	instance.SkinFirst = 17;
	instance.SkinCount = 6;

	std::array<MeshEntry, 4> mesh = {
		Mesh(Vector3(2.0f, 0.0f, -1.0f), Vector3(4.0f, 1.0f, 8.0f), 1000, 0),
		Mesh(Vector3(-5.0f, 7.0f, 2.0f), Vector3(2.0f, 6.0f, 1.0f), 500, 100),
		Mesh(Vector3(0.0f, 4.0f, 0.0f), Vector3(1.0f, 2.0f, 4.0f), 250, 200),
		Mesh(Vector3(3.0f, -2.0f, 9.0f), Vector3(6.0f, 3.0f, 2.0f), 125, 300),
	};
	for (MeshEntry &level : mesh) {
		level.JointCount = 6;
	}
	const std::array<const MeshEntry *, 4> levels = {&mesh[0], &mesh[1], &mesh[2], &mesh[3]};

	LodPlan plan;
	REQUIRE(AppendAuthoredLod(plan, 12, instance, levels));
	REQUIRE(plan.Instances.size() == 4);
	REQUIRE(plan.Draws.size() == 1);
	CHECK(plan.Draws[0].Slot == 12);
	CHECK(plan.Draws[0].LevelCount == 4);
	CHECK(plan.Selections[0].Triangles == glm::uvec4(1000, 500, 250, 125));
	CHECK(plan.Selections[0].CentreTarget.w == Approx(9.0f));

	for (size_t level = 0; level < mesh.size(); ++level) {
		const glm::mat4 model = ModelMatrixOf(plan.Instances[level]);
		const glm::vec4 centre =
			model * glm::vec4{mesh[level].Centre.X, mesh[level].Centre.Y, mesh[level].Centre.Z, 1.0f};
		CHECK(centre.x == Approx(8.0f));
		CHECK(centre.y == Approx(-3.0f));
		CHECK(centre.z == Approx(20.0f));
		CHECK(plan.Instances[level].Scale.x * mesh[level].Extent.X == Approx(2.0f));
		CHECK(plan.Instances[level].Scale.y * mesh[level].Extent.Y == Approx(3.0f));
		CHECK(plan.Instances[level].Scale.z * mesh[level].Extent.Z == Approx(4.0f));
		CHECK(plan.Indices[level] == level);
		CHECK(plan.SkinOffsets[level] == 17);
	}
}

TEST_CASE("material runs become consecutive indirect arguments per level", "[render][lod]") {
	DrawInstance instance;
	instance.LodStrategyMode = LodStrategy::Authored;
	instance.LodLevels = 2;

	MeshEntry detailed = Mesh(Vector3(), Vector3(0.5f, 0.5f, 0.5f), 20, 4);
	detailed.Runs = {MeshRange{3, 18, 4}, MeshRange{21, 42, 4}};
	detailed.Textures.resize(2);
	detailed.Colours.resize(2);
	MeshEntry coarse = Mesh(Vector3(), Vector3(0.5f, 0.5f, 0.5f), 6, 90);
	const std::array<const MeshEntry *, 2> levels = {&detailed, &coarse};

	LodPlan plan;
	REQUIRE(AppendAuthoredLod(plan, 2, instance, levels));
	REQUIRE(plan.Commands.size() == 3);
	CHECK(plan.Draws[0].Levels[0].FirstArgument == 0);
	CHECK(plan.Draws[0].Levels[0].ArgumentCount == 2);
	CHECK(plan.Draws[0].Levels[1].FirstArgument == 2);
	CHECK(plan.Draws[0].Levels[1].ArgumentCount == 1);
	CHECK(plan.Commands[0].num_instances == 0);
	CHECK(plan.Commands[2].first_instance == 1);

	const auto layout = TransferLayoutOf(plan);
	CHECK(layout.Selections == 0);
	CHECK(layout.Instances == sizeof(engine::render::GpuLodSelection));
	CHECK(layout.Indices == layout.Instances + 2 * sizeof(engine::render::GpuInstance));
	CHECK(layout.SkinOffsets == layout.Indices + 2 * sizeof(uint32_t));
	CHECK(layout.Arguments == layout.SkinOffsets + 2 * sizeof(uint32_t));
	CHECK(layout.Bytes == layout.Arguments + 3 * sizeof(SDL_GPUIndexedIndirectDrawCommand));
}

TEST_CASE("plain visuals do not allocate an LOD draw", "[render][lod]") {
	DrawInstance instance;
	MeshEntry first = Mesh(Vector3(), Vector3(0.5f, 0.5f, 0.5f), 12, 0);
	MeshEntry second = Mesh(Vector3(), Vector3(0.5f, 0.5f, 0.5f), 6, 0);
	const std::array<const MeshEntry *, 2> levels = {&first, &second};
	LodPlan plan;

	CHECK_FALSE(AppendAuthoredLod(plan, 0, instance, levels));
	CHECK(plan.Commands.empty());
	CHECK(plan.Instances.empty());
	CHECK(plan.SkinOffsets.empty());
}
