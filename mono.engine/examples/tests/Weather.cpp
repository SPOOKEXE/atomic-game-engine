// The weather scene is executable asset coverage for local fog volumes,
// computed environment providers, weather transitions and the control surface.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Input.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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

	engine::ecs::Entity NamedInstance(engine::ecs::Store &store, std::string_view name) {
		engine::ecs::Entity found;
		store.EachEntity([&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) == engine::core::Name(name)) found = entity;
		});
		return found;
	}
	bool HasNamedInstance(engine::ecs::Store &store, std::string_view name) {
		return NamedInstance(store, name) != engine::ecs::NULL_ENTITY;
	}
}

TEST_CASE("weather preset controls write canonical wind and precipitation", "[examples][weather]") {
	const StagedAssets assets;
	engine::ecs::Store store("weather.presets");
	engine::ecs::Scheduler systems;
	std::shared_ptr<engine::script::Runtime> runtime;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("Weather.luau"), error, &runtime
		)
	);
	REQUIRE(runtime);
	const auto provider = NamedInstance(store, "Weather Clouds");
	REQUIRE(provider != engine::ecs::NULL_ENTITY);
	struct Preset {
		const char *Button;
		float Wind;
		float Precipitation;
		float Lightning;
	};
	for (const Preset preset :
		 {Preset{"Weather CLEAR", 4, 0, 0},
		  Preset{"Weather TEMPEST", 24, 150, 1.4f},
		  Preset{"Weather RAIN", 13, 38, 0.25f}}) {
		CAPTURE(preset.Button);
		const auto button = NamedInstance(store, preset.Button);
		REQUIRE(button != engine::ecs::NULL_ENTITY);
		engine::gui::GuiEvent click;
		click.Kind = engine::gui::EventKind::Activated;
		click.Instance = button;
		runtime->DeliverGuiEvents(std::span(&click, 1));
		REQUIRE(runtime->Heartbeat(0));
		const auto *weather = store.Get<engine::scene::WeatherCompute>(provider);
		REQUIRE(weather);
		CHECK(weather->WindMetresPerSecond.Magnitude() == Catch::Approx(preset.Wind));
		CHECK(weather->WindMetresPerSecond.Y == 0);
		CHECK(weather->PrecipitationRate == Catch::Approx(preset.Precipitation));
		CHECK(weather->LightningRate == Catch::Approx(preset.Lightning));
	}
}

TEST_CASE(
	"the weather scene authors WeatherCompute and local volumetric fog", "[examples][weather][volume]"
) {
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
	CHECK(lighting.EnvironmentState.HasWeatherCompute);
	CHECK(lighting.EnvironmentState.Skybox == engine::scene::SkyboxSource::Compute);
	CHECK(lighting.EnvironmentState.Weather.Mode == engine::scene::WeatherMode::Stateful);
	CHECK(lighting.EnvironmentState.Weather.LayerName == engine::core::Name("weather-demo"));
	CHECK(lighting.EnvironmentState.Weather.CellSizeMetres == Catch::Approx(64.0f));
	CHECK(lighting.EnvironmentState.Weather.BaseHeightMetres == Catch::Approx(1024.0f));
	CHECK(lighting.EnvironmentState.Weather.ThicknessMetres == Catch::Approx(1024.0f));
	CHECK((lighting.EnvironmentState.Weather.ExtentMetres == engine::core::Vector3{2048, 1024, 2048}));
	CHECK(lighting.EnvironmentState.Weather.Coverage == Catch::Approx(0.72f));
	CHECK(lighting.EnvironmentState.Weather.FogStrength == Catch::Approx(0.48f));
	CHECK(lighting.EnvironmentState.Weather.PrecipitationRate == Catch::Approx(38.0f));
	CHECK(lighting.EnvironmentState.Weather.WindMetresPerSecond.Magnitude() == Catch::Approx(13.0f));
	CHECK(lighting.EnvironmentState.Weather.AtmosphereDensityStrength == Catch::Approx(0.42f));
	CHECK(lighting.EnvironmentState.Weather.AtmosphereHazeStrength == Catch::Approx(0.55f));
	CHECK(lighting.EnvironmentState.Weather.AtmosphereGlareSuppression == Catch::Approx(0.38f));

	size_t weatherLayerCount = 0;
	store.Each<const engine::scene::WeatherCompute>([&](engine::ecs::Entity provider,
														const engine::scene::WeatherCompute &weather) {
		weatherLayerCount++;
		CHECK(store.InstanceNameOf(provider) == engine::core::Name("Weather Clouds"));
		CHECK(
			engine::ecs::Classes::Describe(store.ClassOf(provider)).Parent ==
			engine::ecs::Classes::Find(engine::core::Name("CloudCompute"))
		);
		CHECK(store.Has<engine::scene::Clouds>(provider));
		CHECK(store.Has<engine::scene::CloudCompute>(provider));
		CHECK(weather.Mode == engine::scene::WeatherMode::Stateful);
		CHECK(weather.TickRate == 20);
		CHECK(weather.MaximumResidentBricks == 512);
		CHECK(weather.ShadowsEnabled);
		CHECK(weather.ShadowCascades == 2);
		CHECK_FALSE(weather.Paused);
	});
	CHECK(weatherLayerCount == 1);

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
	CHECK(HasNamedInstance(store, "Weather Valley East Ridge"));
	CHECK(HasNamedInstance(store, "Weather Valley West Ridge"));
	CHECK(HasNamedInstance(store, "Weather Precipitation Roof"));
	CHECK(HasNamedInstance(store, "Weather Roof Receiver"));
	CHECK(HasNamedInstance(store, "Weather Scene Ground"));
	CHECK(HasNamedInstance(store, "Weather Scene Inside Clouds"));
	CHECK(HasNamedInstance(store, "Weather Scene Above Clouds"));
	CHECK(HasNamedInstance(store, "Weather Scene Valley"));
	CHECK(HasNamedInstance(store, "Weather Scene Roof"));
	CHECK(HasNamedInstance(store, "Weather Scene Storm"));
	CHECK(HasNamedInstance(store, "Weather Stateful Checkpoint"));
	CHECK(HasNamedInstance(store, "Weather Pause Solver"));
}
