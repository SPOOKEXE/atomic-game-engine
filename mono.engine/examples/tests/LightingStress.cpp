// The high-count lighting course must keep all authored inputs while the
// renderer selects its bounded per-camera payload.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_SUITE_ID("engine.examples.lighting-stress")
TEST_DEPENDS("engine.scripthost.scripting")

namespace {
	constexpr size_t LIGHTS_PER_KIND = 256;
	constexpr size_t VOLUME_COUNT = 256;

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
	"lighting stress scene retains high-count lights and fog with one environment",
	"[examples][lighting-stress]"
) {
	const StagedAssets assets;
	engine::ecs::Store store("lighting.stress");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("LightingStress.luau"), error
		)
	);

	const engine::ecs::ClassId point = engine::ecs::Classes::Find(engine::core::Name("PointLight"));
	const engine::ecs::ClassId spot = engine::ecs::Classes::Find(engine::core::Name("SpotLight"));
	size_t pointCount = 0;
	size_t spotCount = 0;
	store.Each<const engine::scene::Light>([&](engine::ecs::Entity entity, const engine::scene::Light &) {
		if (store.ClassOf(entity) == point) pointCount++;
		if (store.ClassOf(entity) == spot) spotCount++;
	});
	CHECK(pointCount == LIGHTS_PER_KIND);
	CHECK(spotCount == LIGHTS_PER_KIND);

	size_t volumeCount = 0;
	store.Each<const engine::scene::Volume>([&](engine::ecs::Entity, const engine::scene::Volume &volume) {
		volumeCount += volume.Enabled ? 1 : 0;
	});
	CHECK(volumeCount == VOLUME_COUNT);

	const engine::scene::WorldLighting lighting = engine::scene::LightingOf(store);
	CHECK(lighting.BloomIntensity == Catch::Approx(1.25f));
	CHECK(lighting.DepthOfFieldIntensity == Catch::Approx(0.7f));
	CHECK(lighting.GodRayIntensity == Catch::Approx(0.8f));
	CHECK(lighting.EnvironmentState.HasAtmosphereCompute);
	CHECK(lighting.EnvironmentState.HasCloudCompute);
	CHECK(lighting.EnvironmentState.CloudVolume.Steps == 24);
}
