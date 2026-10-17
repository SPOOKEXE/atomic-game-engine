#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.depthoffieldgodrays")
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

TEST_CASE(
	"the lighting effects scene combines sun, clouds, fog and HDR controls", "[examples][scene][lighting]"
) {
	const StagedAssets assets;
	engine::ecs::Store store("depth_of_field_god_rays");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("DepthOfFieldGodRays.luau"), error
		)
	);

	const engine::scene::WorldLighting lighting = engine::scene::LightingOf(store);
	CHECK(lighting.BloomIntensity == Catch::Approx(0.8f));
	CHECK(lighting.DepthOfFieldIntensity == Catch::Approx(0.85f));
	CHECK(lighting.DepthOfFieldFocusDistance == Catch::Approx(24.0f));
	CHECK(lighting.GodRayIntensity == Catch::Approx(0.7f));
	CHECK(lighting.GodRayRadius == Catch::Approx(64.0f));
	CHECK(lighting.EnvironmentState.HasCloudCompute);
}
