// The LOD decimation scene's authored sources and observer camera.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.loddecimation")
TEST_DEPENDS("engine.scripthost.scripting")

namespace {
	using engine::ecs::Entity;
	using engine::ecs::Store;

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	Entity Child(Store &store, const char *name) {
		return store.FindFirstChild(engine::scene::WorkspaceOf(store), name);
	}

	void CheckAutomaticDecimation(const engine::scene::AutoMeshLOD &lod) {
		CHECK(lod.Strategy == engine::scene::LodStrategy::Decimated);
		CHECK(lod.Levels == 3);
		CHECK(lod.Ratios[0] == 0.35f);
		CHECK(lod.Ratios[1] == 0.12f);
		CHECK(lod.TargetQuadArea == 8.0f);
		CHECK_FALSE(lod.Meshes[0].IsValid());
		CHECK_FALSE(lod.Meshes[1].IsValid());
	}
}

TEST_CASE(
	"LOD decimation scene requests generated levels for normal and editable mesh sources", "[examples][lod]"
) {
	const StagedAssets assets;
	Store store("examples.lod_decimation");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("LodDecimation.luau"), error
		)
	);

	const Entity normal = Child(store, "NormalMeshLodSource");
	const Entity editablePart = Child(store, "EditableMeshLodSource");
	const Entity mainCamera = Child(store, "LodMainCamera");
	const Entity observerCamera = Child(store, "LodObserverCamera");
	REQUIRE(normal != engine::ecs::NULL_ENTITY);
	REQUIRE(editablePart != engine::ecs::NULL_ENTITY);
	REQUIRE(mainCamera != engine::ecs::NULL_ENTITY);
	REQUIRE(observerCamera != engine::ecs::NULL_ENTITY);

	const auto *normalVisual = store.Get<engine::scene::Visual>(normal);
	const auto *editableVisual = store.Get<engine::scene::Visual>(editablePart);
	const auto *normalLod = store.Get<engine::scene::AutoMeshLOD>(normal);
	const auto *editableLod = store.Get<engine::scene::AutoMeshLOD>(editablePart);
	REQUIRE(normalVisual != nullptr);
	REQUIRE(editableVisual != nullptr);
	REQUIRE(normalLod != nullptr);
	REQUIRE(editableLod != nullptr);
	CHECK(normalVisual->Mesh == engine::core::Name("engine.Sphere"));
	CHECK(editableVisual->Mesh.IsValid());
	CHECK(editableVisual->Mesh != normalVisual->Mesh);
	CheckAutomaticDecimation(*normalLod);
	CheckAutomaticDecimation(*editableLod);

	const engine::scene::LevelOfDetail normalLevels =
		engine::scene::ResolveMeshLOD(normalVisual->Mesh, normalLod, nullptr);
	const engine::scene::LevelOfDetail editableLevels =
		engine::scene::ResolveMeshLOD(editableVisual->Mesh, editableLod, nullptr);
	CHECK(normalLevels.Levels == 3);
	CHECK(editableLevels.Levels == 3);
	CHECK(normalLevels.Meshes[0] != normalVisual->Mesh);
	CHECK(editableLevels.Meshes[0] != editableVisual->Mesh);

	size_t editableMeshes = 0;
	size_t editableTriangles = 0;
	store.Each<const engine::scene::EditableMesh>([&](Entity, const engine::scene::EditableMesh &mesh) {
		editableMeshes++;
		editableTriangles += mesh.Indices.size() / 3;
	});
	CHECK(editableMeshes == 1);
	CHECK(editableTriangles == 24 * 24 * 2);

	engine::scene::MeshCatalogue observerCatalogue;
	observerCatalogue.Triangles[normalVisual->Mesh.Id()] = 720;
	observerCatalogue.Triangles[editableVisual->Mesh.Id()] = static_cast<uint32_t>(editableTriangles);
	CHECK(engine::scene::SelectLevel(normalLevels, observerCatalogue, normalVisual->Mesh, 50000.0f) == 0);
	CHECK(engine::scene::SelectLevel(editableLevels, observerCatalogue, editableVisual->Mesh, 50000.0f) == 0);
	CHECK(engine::scene::SelectLevel(normalLevels, observerCatalogue, normalVisual->Mesh, 1000.0f) == 2);
	CHECK(engine::scene::SelectLevel(editableLevels, observerCatalogue, editableVisual->Mesh, 1000.0f) == 2);

	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(active->Entity == mainCamera);
	const auto *main = store.Get<engine::scene::Camera>(mainCamera);
	const auto *observer = store.Get<engine::scene::Camera>(observerCamera);
	const auto *mainFrame = store.Get<engine::scene::Transform>(mainCamera);
	const auto *observerFrame = store.Get<engine::scene::Transform>(observerCamera);
	REQUIRE(main != nullptr);
	REQUIRE(observer != nullptr);
	REQUIRE(mainFrame != nullptr);
	REQUIRE(observerFrame != nullptr);
	CHECK(main->FieldOfViewRadians < observer->FieldOfViewRadians);
	CHECK(observer->FarPlane == 180.0f);
	CHECK(mainFrame->Frame.Position.Y == 24.0f);
	CHECK(observerFrame->Frame.Position.Y == 70.0f);
}
