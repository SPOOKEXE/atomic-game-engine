// The black-hole camera keeps its nearby visual field inside its frustum.

#include <engine/core/Paths.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.blackholesimulator")
TEST_DEPENDS("engine.graph.frustum")
TEST_DEPENDS("engine.scripthost.scripting")

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

TEST_CASE("the black-hole camera keeps close visual detail beyond its near plane", "[examples][black-hole]") {
	const StagedAssets assets;
	engine::ecs::Store store("black-hole");
	engine::ecs::Scheduler systems;

	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("BlackHoleSimulator.luau"), error
		)
	);

	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(active != nullptr);
	const auto *camera = store.Get<engine::scene::Camera>(active->Entity);
	REQUIRE(camera != nullptr);

	// The eye is five centimetres above the horizon. A local particle at three
	// centimetres must remain in the culling volume; the former default clipped it.
	const engine::core::CFrame eye =
		engine::core::CFrame::LookAt(engine::core::Vector3{0.0f, 0.0f, 5.55f}, engine::core::Vector3::Zero);
	const engine::core::AABB nearby = engine::core::AABB::FromCentre(
		engine::core::Vector3{0.0f, 0.0f, 5.52f}, engine::core::Vector3{0.005f, 0.005f, 0.005f}
	);
	const engine::graph::Frustum closeFrustum = engine::graph::Frustum::FromViewProjection(
		engine::scene::ResolveCamera(eye, *camera, 16.0f / 9.0f).ViewProjection
	);
	CHECK(closeFrustum.Intersects(nearby));

	engine::scene::Camera defaultCamera = *camera;
	defaultCamera.NearPlane = 0.1f;
	const engine::graph::Frustum defaultFrustum = engine::graph::Frustum::FromViewProjection(
		engine::scene::ResolveCamera(eye, defaultCamera, 16.0f / 9.0f).ViewProjection
	);
	CHECK_FALSE(defaultFrustum.Intersects(nearby));
}
