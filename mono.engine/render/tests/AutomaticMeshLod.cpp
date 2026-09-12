#include <engine/assets/Builtin.hpp>
#include <engine/render/AutomaticMeshLod.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.automaticmeshlod")
TEST_DEPENDS("engine.assets.mesh-decimate")
TEST_DEPENDS("engine.scene.levelofdetail")

TEST_CASE("automatic mesh LOD planning builds and shares real artifacts", "[render][lod][automatic]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::Universe universe;
	const world::WorldId first = universe.Create({.Name = core::Name("lod.first")});
	const world::WorldId second = universe.Create({.Name = core::Name("lod.second")});
	const core::Name base("lod.base");
	scene::AutoMeshLOD policy;
	policy.Strategy = scene::LodStrategy::Decimated;
	policy.Levels = 3;
	policy.Ratios[0] = 0.5f;
	policy.Ratios[1] = 0.25f;

	for (const world::WorldId id : {first, second}) {
		universe.Enter(id, [&](ecs::Store &store) {
			const ecs::Entity part = store.Create();
			scene::Visual visual;
			visual.Mesh = base;
			store.Set(part, visual);
			store.Set(part, policy);
		});
	}

	const assets::MeshData source = assets::MakeBuiltin(assets::BuiltinMesh::Cube);
	const std::array worlds{first, second};
	const auto artifacts = render::BuildAutomaticMeshLods(universe, worlds, base, source);
	REQUIRE(artifacts.size() == 2);
	for (size_t slot = 0; slot < artifacts.size(); slot++) {
		const auto &artifact = artifacts[slot];
		CHECK(
			artifact.Name ==
			scene::AutoMeshLodArtifactName(base, static_cast<uint8_t>(slot + 1), policy.Ratios[slot])
		);
		CHECK(artifact.Data.IsValid());
		CHECK(artifact.Data.Indices.size() < source.Indices.size());
		CHECK(artifact.Worlds.size() == 2);
	}
}

TEST_CASE(
	"reduced automatic mesh LOD planning publishes separate area-weighted artifacts",
	"[render][lod][automatic]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::Universe universe;
	const world::WorldId world = universe.Create({.Name = core::Name("lod.reduced")});
	const core::Name base("lod.reduced-base");
	scene::AutoMeshLOD policy;
	policy.Strategy = scene::LodStrategy::Reduced;
	policy.Levels = 2;
	policy.Ratios[0] = 0.5f;

	universe.Enter(world, [&](ecs::Store &store) {
		const ecs::Entity part = store.Create();
		scene::Visual visual;
		visual.Mesh = base;
		store.Set(part, visual);
		store.Set(part, policy);
	});

	const assets::MeshData source = assets::MakeBuiltin(assets::BuiltinMesh::Cube);
	const std::array worlds{world};
	const auto artifacts = render::BuildAutomaticMeshLods(universe, worlds, base, source);
	REQUIRE(artifacts.size() == 1);
	CHECK(artifacts[0].Name == scene::AutoMeshLodArtifactName(base, 1, 0.5f, scene::LodStrategy::Reduced));
	CHECK(artifacts[0].Data.IsValid());
	CHECK(artifacts[0].Data.Indices.size() < source.Indices.size());
}
