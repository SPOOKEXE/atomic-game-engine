// The authored lighting course crosses the real world-to-renderer collector.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.render.lighting-stress")
TEST_DEPENDS("engine.examples.lighting-stress")

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};
}

TEST_CASE("lighting stress scene reaches bounded camera lighting payloads", "[render][lighting-stress]") {
	const StagedAssets assets;
	engine::ecs::Store store("render.lighting.stress");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("LightingStress.luau"), error
		)
	);

	engine::render::RegisterPresentationComponents();
	store.SetResource(engine::render::DrawList{});
	REQUIRE(engine::scene::SyncRendered(store) > 0);
	store.SetResource(engine::render::DrawList{});
	engine::render::CollectInstances(store);
	const auto *draw = store.Resource<engine::render::DrawList>();
	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(draw != nullptr);
	REQUIRE(active != nullptr);
	const auto *transform = store.Get<engine::scene::Transform>(active->Entity);
	const auto *camera = store.Get<engine::scene::Camera>(active->Entity);
	REQUIRE(transform != nullptr);
	REQUIRE(camera != nullptr);

	engine::render::View view;
	view.CameraFrame = transform->Frame;
	view.Camera = *camera;
	view.Instances = draw->Instances;
	engine::render::WorldCameraFrame collected;
	engine::render::CollectWorldCamera(store, view, {1280, 720}, collected);
	CHECK(collected.Lights.size() == engine::render::MAX_SCENE_LIGHTS);
	CHECK(collected.VolumeCount == engine::scene::MAX_SCENE_VOLUMES);
}
