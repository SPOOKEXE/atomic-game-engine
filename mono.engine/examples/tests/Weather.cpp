// The weather scene is executable asset coverage for local fog volumes,
// computed environment providers, weather transitions and the control surface.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.weather")
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

TEST_CASE("the weather scene stages and authors local volumetric fog", "[examples][weather][volume]") {
	const StagedAssets assets;
	const engine::examples::DemosLoader demos;
	CHECK(demos.Find(engine::examples::DemoKind::Script, "Weather.luau").has_value());

	engine::ecs::Store store("weather");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(store, systems, engine::examples::ExamplePath("Weather.luau"), error)
	);

	const engine::scene::WorldLighting lighting = engine::scene::LightingOf(store);
	CHECK(lighting.FogStart == Catch::Approx(0.0f));
	CHECK(lighting.FogEnd == Catch::Approx(100000.0f));
	CHECK(lighting.BloomThreshold == Catch::Approx(0.8f));
	CHECK(lighting.EnvironmentState.HasAtmosphereCompute);
	CHECK(lighting.EnvironmentState.HasCloudCompute);
	CHECK(lighting.EnvironmentState.Skybox == engine::scene::SkyboxSource::Compute);

	size_t volumeCount = 0;
	bool hasSquashedEllipsoid = false;
	store.Each<const engine::scene::Volume>([&](engine::ecs::Entity, const engine::scene::Volume &volume) {
		volumeCount++;
		hasSquashedEllipsoid = hasSquashedEllipsoid ||
							   (volume.Shape == engine::scene::VolumeShape::Ellipsoid &&
								volume.HalfExtent.Y < volume.HalfExtent.X && volume.NoiseStrength > 0.7f);
	});
	CHECK(volumeCount == 2);
	CHECK(hasSquashedEllipsoid);
}
