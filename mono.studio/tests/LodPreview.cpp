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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <studio/Projection.hpp>

TEST_SUITE_ID("studio.lodpreview")
TEST_DEPENDS("engine.scene.levelofdetail")

using Catch::Approx;

namespace {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::Bounds;
	using engine::scene::Camera;
	using engine::scene::CameraMatrices;
	using engine::scene::LODAuto;
	using engine::scene::LODSettings;
	using engine::scene::MeshCatalogue;
	using engine::scene::Transform;
	using engine::scene::Visual;

	studio::PanelProjection
	Panel(Vector3 eye = {}, glm::vec2 imageSize = {800.0f, 400.0f}, glm::vec2 renderSize = {}) {
		if (renderSize.x <= 0.0f || renderSize.y <= 0.0f) {
			renderSize = imageSize;
		}
		Camera camera;
		const CameraMatrices matrices =
			engine::scene::ResolveCamera(CFrame(eye), camera, renderSize.x / renderSize.y);
		studio::PanelProjection panel;
		panel.Matrix = matrices.ViewProjection;
		panel.Eye = eye;
		panel.ImageSize = imageSize;
		panel.RenderSize = renderSize;
		return panel;
	}

	Entity MeshPart(Store &store) {
		const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Statue");
		Visual visual;
		visual.Mesh = Name("studio.lod-preview.base");
		store.Set(part, visual);
		store.Set(part, Transform{CFrame{Vector3{0.0f, 0.0f, -10.0f}}});
		store.Set(part, Bounds{Vector3{1.0f, 1.0f, 1.0f}});
		LODAuto lod;
		lod.Meshes[0] = Name("studio.lod-preview.half");
		lod.Levels = 2;
		store.Set(part, lod);
		return part;
	}
}

TEST_CASE("the active LOD label is horizontally centred over the object", "[studio][lod]") {
	const float labelX = studio::CenteredLodLabelX(100.0f, 300.0f, 80.0f);
	CHECK(labelX == Approx(160.0f));
	CHECK(labelX + 40.0f == Approx(200.0f));
}

TEST_CASE(
	"the focused viewport reports a mesh part's active lod from known triangle metadata", "[studio][lod]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview");
	const Entity part = MeshPart(store);
	store.GetMutable<LODAuto>(part)->TargetQuadArea = 0.001f;
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
	store.Remove<LODAuto>(part);
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	store.SetResource(catalogue);

	CHECK(studio::ActiveLodForViewport(store, part, Panel(), {30.0f, 60.0f, 120.0f}) == 0);
}

TEST_CASE("visible MeshPart labels survive switching between worlds", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store first("studio_lod_label_first");
	Store second("studio_lod_label_second");
	const Entity firstPart = MeshPart(first);
	const Entity secondPart = MeshPart(second);
	first.Remove<LODAuto>(firstPart);
	second.Remove<LODAuto>(secondPart);

	CHECK(studio::ShouldDrawActiveLodLabel(first, firstPart));
	CHECK(studio::ShouldDrawActiveLodLabel(second, secondPart));
	CHECK(studio::ShouldDrawActiveLodLabel(first, firstPart));

	first.GetMutable<Visual>(firstPart)->Visible = false;
	CHECK_FALSE(studio::ShouldDrawActiveLodLabel(first, firstPart));
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

	const auto level = studio::ActiveLodForViewport(store, part, Panel(), {30.0f, 60.0f, 120.0f});
	REQUIRE(level.has_value());
	CHECK(*level == 1);
}

TEST_CASE("distance preferences force the preview onto a coarse resident lod", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_distance_floor");
	const Entity part = MeshPart(store);
	store.GetMutable<LODAuto>(part)->TargetQuadArea = 0.001f;
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	const auto level = studio::ActiveLodForViewport(store, part, Panel(), {1.0f, 2.0f, 3.0f});
	REQUIRE(level.has_value());
	CHECK(*level == 1);
}

TEST_CASE(
	"per-item LOD distances override Studio defaults only as one complete ordered set", "[studio][lod]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_item_distances");
	const Entity part = MeshPart(store);
	store.GetMutable<LODAuto>(part)->TargetQuadArea = 0.001f;
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	LODSettings settings;
	settings.MinimumDistances[0] = 1.0f;
	settings.MinimumDistances[1] = 2.0f;
	settings.MinimumDistances[2] = 3.0f;
	store.Set(part, settings);
	CHECK(
		studio::EffectiveLodDistanceBands(&settings, {100.0f, 200.0f, 300.0f}) ==
		std::array<float, 3>{1.0f, 2.0f, 3.0f}
	);
	CHECK(
		studio::EditedLodDistanceBands(&settings, {100.0f, 200.0f, 300.0f}, 1, 20.0f) ==
		std::array<float, 3>{1.0f, 20.0f, 3.0f}
	);
	CHECK(studio::ActiveLodForViewport(store, part, Panel(), {100.0f, 200.0f, 300.0f}) == 1);

	settings.MinimumDistances[1] = 0.0f;
	store.Set(part, settings);
	CHECK(
		studio::EffectiveLodDistanceBands(&settings, {100.0f, 200.0f, 300.0f}) ==
		std::array<float, 3>{100.0f, 200.0f, 300.0f}
	);
	CHECK(
		studio::EditedLodDistanceBands(&settings, {100.0f, 200.0f, 300.0f}, 1, 150.0f) ==
		std::array<float, 3>{100.0f, 150.0f, 300.0f}
	);
	CHECK(studio::ActiveLodForViewport(store, part, Panel(), {100.0f, 200.0f, 300.0f}) == 0);
}

TEST_CASE("the preview measures LOD area in render-target pixels", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_target_pixels");
	const Entity part = MeshPart(store);
	store.GetMutable<LODAuto>(part)->TargetQuadArea = 1.0f;
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	const studio::PanelProjection panel = Panel({}, {800.0f, 400.0f}, {1600.0f, 800.0f});
	const auto largeTarget = studio::ActiveLodForViewport(store, part, panel, {100.0f, 200.0f, 300.0f});
	REQUIRE(largeTarget.has_value());
	CHECK(*largeTarget == 0);

	// The same displayed rectangle at one quarter of the target pixels cannot
	// keep the base mesh above the target quad area.
	const studio::PanelProjection smallerTarget = Panel({}, {800.0f, 400.0f}, {800.0f, 400.0f});
	const auto smallTarget =
		studio::ActiveLodForViewport(store, part, smallerTarget, {100.0f, 200.0f, 300.0f});
	REQUIRE(smallTarget.has_value());
	CHECK(*smallTarget == 1);
}

TEST_CASE("moving toward and away from a mesh updates its distance-floor lod", "[studio][lod]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("studio_lod_preview_bidirectional_distance");
	const Entity part = MeshPart(store);
	store.GetMutable<LODAuto>(part)->TargetQuadArea = 0.001f;
	MeshCatalogue catalogue;
	catalogue.Triangles[Name("studio.lod-preview.base").Id()] = 10000;
	catalogue.Triangles[Name("studio.lod-preview.half").Id()] = 5000;
	store.SetResource(catalogue);

	const std::array<float, 3> bands{5.0f, 15.0f, 30.0f};
	CHECK(studio::ActiveLodForViewport(store, part, Panel(Vector3{}), bands) == 1);
	CHECK(studio::ActiveLodForViewport(store, part, Panel(Vector3{0.0f, 0.0f, -9.0f}), bands) == 0);
	CHECK(studio::ActiveLodForViewport(store, part, Panel(Vector3{}), bands) == 1);
}
