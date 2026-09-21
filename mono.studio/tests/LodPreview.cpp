// The inspector reports the focused viewport's LOD choice without storing it in ECS.

#include "LodPreview.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/Projection.hpp>

TEST_SUITE_ID("studio.lodpreview")
TEST_DEPENDS("engine.scene.levelofdetail")

namespace {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::AutoMeshLOD;
	using engine::scene::Bounds;
	using engine::scene::Camera;
	using engine::scene::CameraMatrices;
	using engine::scene::MeshCatalogue;
	using engine::scene::Transform;
	using engine::scene::Visual;

	studio::PanelProjection Panel() {
		Camera camera;
		const CameraMatrices matrices = engine::scene::ResolveCamera(CFrame{}, camera, 2.0f);
		studio::PanelProjection panel;
		panel.Matrix = matrices.ViewProjection;
		panel.ImageSize = {800.0f, 400.0f};
		return panel;
	}

	Entity MeshPart(Store &store) {
		const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Statue");
		Visual visual;
		visual.Mesh = Name("studio.lod-preview.base");
		store.Set(part, visual);
		store.Set(part, Transform{CFrame{Vector3{0.0f, 0.0f, -10.0f}}});
		store.Set(part, Bounds{Vector3{1.0f, 1.0f, 1.0f}});
		AutoMeshLOD lod;
		lod.Meshes[0] = Name("studio.lod-preview.half");
		lod.Levels = 2;
		store.Set(part, lod);
		return part;
	}
}

TEST_CASE(
	"the focused viewport reports a mesh part's active lod from known triangle metadata", "[studio][lod]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview");
	const Entity part = MeshPart(store);
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	const auto level = studio::ActiveLodForViewport(store, part, Panel(), {100.0f, 200.0f, 300.0f});
	REQUIRE(level.has_value());
	CHECK(*level == 0);
}

TEST_CASE("the preview reports the resident prefix while coarse lod meshes build", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_missing");
	const Entity part = MeshPart(store);
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	store.SetResource(catalogue);

	CHECK(studio::ActiveLodForViewport(store, part, Panel(), {100.0f, 200.0f, 300.0f}) == 0);
}

TEST_CASE("a base-only editable mesh reports active lod zero", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_base_only");
	const Entity part = MeshPart(store);
	store.Remove<AutoMeshLOD>(part);
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	store.SetResource(catalogue);

	CHECK(studio::ActiveLodForViewport(store, part, Panel(), {30.0f, 60.0f, 120.0f}) == 0);
}

TEST_CASE("a distant mesh part previews its coarser level", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_coarse");
	const Entity part = MeshPart(store);
	store.Set(part, Transform{CFrame{Vector3{0.0f, 0.0f, -500.0f}}});
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	const auto level = studio::ActiveLodForViewport(store, part, Panel(), {1.0f, 2.0f, 3.0f});
	REQUIRE(level.has_value());
	CHECK(*level == 1);
}
