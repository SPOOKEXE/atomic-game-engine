// The weather scene is executable asset coverage for local fog volumes,
// computed environment providers, weather transitions and the control surface.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/LightingEffects.hpp>
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
	CHECK(lighting.FogStart == Catch::Approx(1000000000.0f));
	CHECK(lighting.FogEnd == Catch::Approx(1000001024.0f));
	CHECK(lighting.BloomThreshold == Catch::Approx(0.8f));
	CHECK(lighting.BloomIntensity == Catch::Approx(1.0f));
	CHECK(lighting.BloomRadius == Catch::Approx(8.0f));
	CHECK(lighting.DepthOfFieldFocusDistance == Catch::Approx(54.0f));
	CHECK(lighting.DepthOfFieldFocusRange == Catch::Approx(14.0f));
	CHECK(lighting.DepthOfFieldRadius == Catch::Approx(7.0f));
	CHECK(lighting.DepthOfFieldIntensity == Catch::Approx(0.54f));
	CHECK(lighting.GodRayThreshold == Catch::Approx(0.72f));
	CHECK(lighting.GodRayRadius == Catch::Approx(72.0f));
	CHECK(lighting.GodRayIntensity == Catch::Approx(0.68f));
	size_t bloomCount = 0;
	store.Each<const engine::scene::BloomEffect>([&](engine::ecs::Entity, const auto &effect) {
		bloomCount++;
		CHECK(effect.Enabled);
		CHECK(effect.Threshold == Catch::Approx(0.8f));
		CHECK(effect.Intensity == Catch::Approx(1.0f));
		CHECK(effect.Radius == Catch::Approx(8.0f));
	});
	CHECK(bloomCount == 1);
	size_t depthOfFieldCount = 0;
	store.Each<const engine::scene::DepthOfFieldEffect>([&](engine::ecs::Entity, const auto &effect) {
		depthOfFieldCount++;
		CHECK(effect.Enabled);
		CHECK(effect.FocusDistance == Catch::Approx(54.0f));
		CHECK(effect.FocusRange == Catch::Approx(14.0f));
		CHECK(effect.Radius == Catch::Approx(7.0f));
		CHECK(effect.Intensity == Catch::Approx(0.54f));
	});
	CHECK(depthOfFieldCount == 1);
	size_t godRaysCount = 0;
	store.Each<const engine::scene::GodRaysEffect>([&](engine::ecs::Entity, const auto &effect) {
		godRaysCount++;
		CHECK(effect.Enabled);
		CHECK(effect.Threshold == Catch::Approx(0.72f));
		CHECK(effect.Radius == Catch::Approx(72.0f));
		CHECK(effect.Intensity == Catch::Approx(0.68f));
	});
	CHECK(godRaysCount == 1);
	size_t fogCount = 0;
	store.Each<const engine::scene::FogEffect>([&](engine::ecs::Entity, const auto &) { fogCount++; });
	CHECK(fogCount == 0);
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

	size_t rainEmitterCount = 0;
	store.Each<const engine::effects::ParticleEmitter>([&](engine::ecs::Entity,
														   const engine::effects::ParticleEmitter &emitter) {
		rainEmitterCount++;
		CHECK(emitter.Rate == Catch::Approx(280.0f / 9.0f));
		CHECK(emitter.MaxParticles == 480);
		CHECK(emitter.EmissionDirection == engine::scene::NormalId::Bottom);
		CHECK(emitter.Brightness == Catch::Approx(8.0f));
		CHECK(emitter.Size.Evaluate(0.0f) == Catch::Approx(0.34f));
		CHECK(emitter.Squash.Evaluate(0.0f) == Catch::Approx(8.0f));
	});
	CHECK(rainEmitterCount == 9);
}
