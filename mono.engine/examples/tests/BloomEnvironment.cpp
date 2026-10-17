// The bloom environment scene keeps authored bloom and environment providers
// together so a visual check includes the sun, atmosphere and clouds.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.bloomenvironment")
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
	"the bloom environment scene authors HDR bloom with atmospheric sunlight", "[examples][scene][bloom]"
) {
	const StagedAssets assets;
	engine::ecs::Store store("bloom_environment");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("BloomEnvironment.luau"), error
		)
	);

	const engine::scene::WorldLighting lighting = engine::scene::LightingOf(store);
	CHECK(lighting.BloomThreshold == Catch::Approx(0.8f));
	CHECK(lighting.BloomIntensity == Catch::Approx(1.35f));
	CHECK(lighting.BloomRadius == Catch::Approx(10.0f));
	CHECK(lighting.Direct.R > 0.0f);
	CHECK(lighting.EnvironmentState.HasAtmosphereCompute);
	CHECK(lighting.EnvironmentState.HasCloudCompute);
	CHECK(lighting.EnvironmentState.Skybox == engine::scene::SkyboxSource::Compute);

	size_t orbCount = 0;
	store.EachChild(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity child) {
		if (store.InstanceNameOf(child).Text().starts_with("Bloom Orb ")) {
			orbCount++;
		}
	});
	CHECK(orbCount == 512);
}
