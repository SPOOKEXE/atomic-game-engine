#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/LightingEffects.hpp>
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
	size_t bloomCount = 0;
	store.Each<const engine::scene::BloomEffect>([&](engine::ecs::Entity, const auto &effect) {
		bloomCount++;
		CHECK(effect.Enabled);
		CHECK(effect.Intensity == Catch::Approx(0.8f));
	});
	CHECK(bloomCount == 1);
	size_t depthOfFieldCount = 0;
	store.Each<const engine::scene::DepthOfFieldEffect>([&](engine::ecs::Entity, const auto &effect) {
		depthOfFieldCount++;
		CHECK(effect.Enabled);
		CHECK(effect.Intensity == Catch::Approx(0.85f));
		CHECK(effect.FocusDistance == Catch::Approx(24.0f));
	});
	CHECK(depthOfFieldCount == 1);
	size_t godRaysCount = 0;
	store.Each<const engine::scene::GodRaysEffect>([&](engine::ecs::Entity, const auto &effect) {
		godRaysCount++;
		CHECK(effect.Enabled);
		CHECK(effect.Intensity == Catch::Approx(0.7f));
		CHECK(effect.Radius == Catch::Approx(64.0f));
	});
	CHECK(godRaysCount == 1);
	size_t fogCount = 0;
	store.Each<const engine::scene::FogEffect>([&](engine::ecs::Entity, const auto &effect) {
		fogCount++;
		CHECK(effect.Enabled);
		const engine::core::Color3 expectedColor{98.0f / 255.0f, 129.0f / 255.0f, 166.0f / 255.0f};
		CHECK(effect.Color.R == Catch::Approx(expectedColor.R));
		CHECK(effect.Color.G == Catch::Approx(expectedColor.G));
		CHECK(effect.Color.B == Catch::Approx(expectedColor.B));
		CHECK(lighting.FogColor == effect.Color);
		CHECK(effect.Start == Catch::Approx(18.0f));
		CHECK(effect.End == Catch::Approx(92.0f));
	});
	CHECK(fogCount == 1);
	CHECK(lighting.EnvironmentState.HasCloudCompute);
}
