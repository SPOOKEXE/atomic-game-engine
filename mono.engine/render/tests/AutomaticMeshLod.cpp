#include "RenderFixture.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/render/AutomaticMeshLod.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

TEST_SUITE_ID("engine.render.automaticmeshlod")
TEST_DEPENDS("engine.assets.mesh-decimate")
TEST_DEPENDS("engine.scene.levelofdetail")

namespace {
	engine::assets::MeshData Grid(uint32_t cells) {
		engine::assets::MeshData mesh;
		for (uint32_t y = 0; y <= cells; y++) {
			for (uint32_t x = 0; x <= cells; x++) {
				mesh.Vertices.push_back({{float(x), 0.0f, float(y)}, {0.0f, 1.0f, 0.0f}, {}});
			}
		}
		for (uint32_t y = 0; y < cells; y++) {
			for (uint32_t x = 0; x < cells; x++) {
				const uint32_t a = y * (cells + 1) + x;
				mesh.Indices.insert(
					mesh.Indices.end(), {a, a + 1, a + cells + 1, a + 1, a + cells + 2, a + cells + 1}
				);
			}
		}
		mesh.ComputeBounds();
		return mesh;
	}

	engine::render::PackedMeshData PackedTriangle() {
		using engine::render::PackedMeshData;
		using engine::render::PackedMeshFormat;
		using engine::render::PackedMeshStream;
		PackedMeshData mesh;
		mesh.VertexCount = 3;
		mesh.Indices = {0, 1, 2};
		mesh.Vertices.resize(96);
		mesh.Streams[0] = PackedMeshStream{0, 36, 9, 3, PackedMeshFormat::Float32};
		mesh.Streams[1] = PackedMeshStream{36, 36, 9, 3, PackedMeshFormat::Float32};
		mesh.Streams[2] = PackedMeshStream{72, 24, 6, 2, PackedMeshFormat::Float32};
		mesh.Minimum = {0.0f, 0.0f, 0.0f};
		mesh.Maximum = {1.0f, 1.0f, 0.0f};
		return mesh;
	}

	template <typename Uploader>
	size_t RefreshUntil(Uploader &uploader, engine::ecs::Store &store, engine::render::Renderer &renderer) {
		for (size_t attempt = 0; attempt < 3000; attempt++) {
			if (const size_t uploaded = uploader.Refresh(store, renderer); uploaded != 0) return uploaded;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return 0;
	}
}

TEST_CASE("automatic LOD decimates the editable-demo triangle count", "[render][lod][automatic]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("lod.editable-demo");
	const core::Name base("editable-mesh://lod-demo");
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Strategy = scene::LodStrategy::Decimated;
	policy.Levels = 3;
	policy.Ratios[0] = 0.35f;
	policy.Ratios[1] = 0.12f;
	store.Set(part, policy);

	const assets::MeshData source = Grid(8);
	REQUIRE(source.Indices.size() / 3 == 128);
	const auto artifacts = render::BuildAutomaticMeshLods(store, base, source);
	REQUIRE(artifacts.size() == 2);
	CHECK(artifacts[0].Data.Indices.size() / 3 < 128);
	CHECK(artifacts[1].Data.Indices.size() / 3 < artifacts[0].Data.Indices.size() / 3);
	CHECK(artifacts[0].Name == scene::AutoMeshLodArtifactName(base, 1, 0.35f));
	CHECK(artifacts[1].Name == scene::AutoMeshLodArtifactName(base, 2, 0.12f));
}

TEST_CASE(
	"automatic LOD uploader registers and replaces built-in levels", "[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("lod.builtin-upload");
	const core::Name base(assets::BuiltinName(assets::BuiltinMesh::Sphere));
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Strategy = scene::LodStrategy::Decimated;
	policy.Levels = 3;
	policy.Ratios[0] = 0.35f;
	policy.Ratios[1] = 0.12f;
	store.Set(part, policy);

	render::EditableMeshUploader uploader;
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 2);
	const core::Name first = scene::AutoMeshLodArtifactName(base, 1, 0.35f);
	const core::Name second = scene::AutoMeshLodArtifactName(base, 2, 0.12f);
	const uint32_t baseTriangles = scene::TrianglesOf(store, base);
	const uint32_t firstTriangles = scene::TrianglesOf(store, first);
	const uint32_t secondTriangles = scene::TrianglesOf(store, second);
	CHECK(baseTriangles > firstTriangles);
	CHECK(firstTriangles > secondTriangles);

	const float editedRatio = 0.5f;
	float propertyRatio = 0.0f;
	REQUIRE(store.GetProperty(part, core::Name("AutoLod1Ratio"), &propertyRatio, sizeof(propertyRatio)));
	REQUIRE(store.SetProperty(part, core::Name("AutoLod1Ratio"), &editedRatio, sizeof(editedRatio)));
	CHECK(RefreshUntil(uploader, store, fixture.Render) == 2);
	const core::Name replacement = scene::AutoMeshLodArtifactName(base, 1, 0.5f);
	CHECK(scene::TrianglesOf(store, replacement) > firstTriangles);
	core::Vector3 extent;
	CHECK_FALSE(fixture.Render.MeshExtentOf(first, extent));
}

TEST_CASE(
	"automatic LOD uploader replaces and removes ladders with their source mesh",
	"[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("lod.source-lifecycle");
	const core::Name base("lod.source-lifecycle-base");
	const core::Name artifact = scene::AutoMeshLodArtifactName(base, 1, 0.5f);
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Levels = 2;
	policy.Ratios[0] = 0.5f;
	store.Set(part, policy);
	REQUIRE(fixture.Render.AddMesh(base, Grid(8)));

	render::AutomaticMeshLodUploader uploader;
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	const uint32_t firstTriangles = scene::TrianglesOf(store, artifact);
	REQUIRE(firstTriangles > 0);

	REQUIRE(fixture.Render.AddMesh(base, Grid(10)));
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	CHECK(scene::TrianglesOf(store, artifact) != firstTriangles);

	REQUIRE(fixture.Render.DropMesh(base));
	CHECK(uploader.Refresh(store, fixture.Render) == 0);
	core::Vector3 extent;
	CHECK_FALSE(fixture.Render.MeshExtentOf(artifact, extent));
	CHECK(scene::TrianglesOf(store, artifact) == 0);
}

TEST_CASE(
	"automatic LOD discards work copied before its source is replaced", "[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("lod.source-race");
	const core::Name base("lod.source-race-base");
	const core::Name artifact = scene::AutoMeshLodArtifactName(base, 1, 0.5f);
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Levels = 2;
	policy.Ratios[0] = 0.5f;
	store.Set(part, policy);
	REQUIRE(fixture.Render.AddMesh(base, Grid(14)));

	render::AutomaticMeshLodUploader uploader;
	CHECK(uploader.Refresh(store, fixture.Render) == 0);
	CHECK(uploader.Refresh(store, fixture.Render) == 0);
	const assets::MeshData replacement = Grid(6);
	REQUIRE(fixture.Render.AddMesh(base, replacement));

	const auto expected = render::BuildAutomaticMeshLods(store, base, replacement);
	REQUIRE(expected.size() == 1);
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	CHECK(scene::TrianglesOf(store, artifact) == expected[0].Data.Indices.size() / 3);
}

TEST_CASE(
	"automatic LOD retires its ladder when a packed source cannot be copied",
	"[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("lod.packed-source");
	const core::Name base("lod.packed-source-base");
	const core::Name artifact = scene::AutoMeshLodArtifactName(base, 1, 0.5f);
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Levels = 2;
	policy.Ratios[0] = 0.5f;
	store.Set(part, policy);
	REQUIRE(fixture.Render.AddMesh(base, Grid(8)));

	render::AutomaticMeshLodUploader uploader;
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	REQUIRE(scene::TrianglesOf(store, artifact) > 0);
	REQUIRE(fixture.Render.AddPackedMesh(base, PackedTriangle()));

	CHECK(uploader.Refresh(store, fixture.Render) == 0);
	core::Vector3 extent;
	CHECK_FALSE(fixture.Render.MeshExtentOf(artifact, extent));
	CHECK(scene::TrianglesOf(store, artifact) == 0);
}

TEST_CASE(
	"automatic LOD uploader retains shared-owner levels until every world releases them",
	"[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store first("studio.lod.first"), second("studio.lod.second");
	const core::Name base(assets::BuiltinName(assets::BuiltinMesh::Sphere));
	const core::Name artifact = scene::AutoMeshLodArtifactName(base, 1, 0.35f);
	const auto addPolicy = [&](ecs::Store &store) {
		const ecs::Entity part = store.Create();
		scene::Visual visual;
		visual.Mesh = base;
		store.Set(part, visual);
		scene::LODAuto policy;
		policy.Strategy = scene::LodStrategy::Decimated;
		policy.Levels = 2;
		policy.Ratios[0] = 0.35f;
		store.Set(part, policy);
		return part;
	};
	const ecs::Entity firstPart = addPolicy(first);
	const ecs::Entity secondPart = addPolicy(second);

	render::EditableMeshUploader uploader;
	REQUIRE(RefreshUntil(uploader, first, fixture.Render) == 1);
	CHECK(RefreshUntil(uploader, second, fixture.Render) == 1);
	core::Vector3 extent;
	REQUIRE(fixture.Render.MeshExtentOf(artifact, extent));

	first.Remove<scene::LODAuto>(firstPart);
	CHECK(uploader.RefreshLods(first, fixture.Render) == 0);
	CHECK(fixture.Render.MeshExtentOf(artifact, extent));
	CHECK(scene::TrianglesOf(first, artifact) == 0);
	CHECK(scene::TrianglesOf(second, artifact) > 0);

	second.Remove<scene::LODAuto>(secondPart);
	CHECK(uploader.RefreshLods(second, fixture.Render) == 0);
	CHECK_FALSE(fixture.Render.MeshExtentOf(artifact, extent));
}

TEST_CASE(
	"automatic LOD ignores stale jobs and zero releases published artifacts",
	"[render][gpu][lod][automatic][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("lod.async-boundary");
	const core::Name base("lod.async-source");
	const ecs::Entity part = store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "part");
	scene::Visual visual;
	visual.Mesh = base;
	store.Set(part, visual);
	scene::LODAuto policy;
	policy.Levels = 2;
	policy.Ratios[0] = 0.5f;
	store.Set(part, policy);

	render::AutomaticMeshLodUploader uploader;
	const assets::MeshData source = Grid(8);
	REQUIRE(uploader.RefreshSource(store, fixture.Render, base, source) == 0);
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	const core::Name published = scene::AutoMeshLodArtifactName(base, 1, 0.5f);
	core::Vector3 extent;
	REQUIRE(fixture.Render.MeshExtentOf(published, extent));

	const float editedRatio = 0.25f;
	float propertyRatio = 0.0f;
	REQUIRE(store.GetProperty(part, core::Name("AutoLod1Ratio"), &propertyRatio, sizeof(propertyRatio)));
	REQUIRE(store.SetProperty(part, core::Name("AutoLod1Ratio"), &editedRatio, sizeof(editedRatio)));
	CHECK(uploader.RefreshSource(store, fixture.Render, base, source) == 0);
	REQUIRE(RefreshUntil(uploader, store, fixture.Render) == 1);
	const core::Name republished = scene::AutoMeshLodArtifactName(base, 1, 0.25f);
	CHECK_FALSE(fixture.Render.MeshExtentOf(published, extent));
	REQUIRE(fixture.Render.MeshExtentOf(republished, extent));

	const float tinyRatio = 0.000001f;
	REQUIRE(store.SetProperty(part, core::Name("AutoLod1Ratio"), &tinyRatio, sizeof(tinyRatio)));
	CHECK(uploader.RefreshSource(store, fixture.Render, base, source) == 0);
	const float zeroRatio = 0.0f;
	REQUIRE(store.SetProperty(part, core::Name("AutoLod1Ratio"), &zeroRatio, sizeof(zeroRatio)));
	CHECK(uploader.Refresh(store, fixture.Render) == 0);
	CHECK_FALSE(fixture.Render.MeshExtentOf(published, extent));
	CHECK_FALSE(fixture.Render.MeshExtentOf(scene::AutoMeshLodArtifactName(base, 1, 0.000001f), extent));
}

TEST_CASE("automatic mesh LOD planning builds and shares real artifacts", "[render][lod][automatic]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::Universe universe;
	const world::WorldId first = universe.Create({.Name = core::Name("lod.first")});
	const world::WorldId second = universe.Create({.Name = core::Name("lod.second")});
	const core::Name base("lod.base");
	scene::LODAuto policy;
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
		// The built-in cube splits every face at UV and normal seams. Those
		// boundaries stay fixed, so its safe best-effort result may be unchanged.
		CHECK(artifact.Data.Indices.size() <= source.Indices.size());
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
	scene::LODAuto policy;
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
	CHECK(artifacts[0].Data.Indices.size() <= source.Indices.size());
}
