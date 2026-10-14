// The weather scene is executable asset coverage for local fog volumes,
// computed environment providers, weather transitions and the control surface.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
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
#include <engine/weather/Consumers.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.examples.weather")
TEST_DEPENDS("engine.scripthost.scripting")
TEST_DEPENDS("engine.weather.consumers")

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

	void ActivateControl(
		engine::ecs::Store &store, engine::script::Runtime &runtime, std::string_view name, double seconds = 0
	) {
		const auto button = NamedInstance(store, name);
		REQUIRE(button != engine::ecs::NULL_ENTITY);
		engine::gui::GuiEvent click;
		click.Kind = engine::gui::EventKind::Activated;
		click.Instance = button;
		runtime.DeliverGuiEvents(std::span(&click, 1));
		const bool completed = runtime.Heartbeat(seconds);
		INFO(runtime.LastError());
		REQUIRE(completed);
	}

	engine::core::CFrame CameraFrame(const engine::ecs::Store &store, engine::ecs::Entity camera) {
		engine::ecs::AttributeValue value;
		REQUIRE(engine::ecs::GetAttribute(store, camera, engine::core::Name("CFrame"), value));
		REQUIRE(value.Type == engine::ecs::PropertyType::CFrame);
		return value.CFrame;
	}
}

TEST_CASE("weather controls select six recipes and reachable layer views", "[examples][weather]") {
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
	const auto camera = NamedInstance(store, "DemoCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	engine::ecs::AttributeValue farPlane;
	REQUIRE(engine::ecs::GetAttribute(store, camera, engine::core::Name("FarPlaneZ"), farPlane));
	REQUIRE(farPlane.Type == engine::ecs::PropertyType::Double);
	CHECK(farPlane.Double == 6000);
	using CloudShader = engine::scene::CloudComputeShader;
	struct Preset {
		const char *Button;
		float Wind;
		float Precipitation;
		float Lightning;
		float BaseHeight;
		float Thickness;
		uint32_t FeatureCells;
		float Temperature;
		float Vapour;
		float Ice;
		float Hail;
		float Lift;
		float Coverage;
		float Cover;
		float Density;
		CloudShader Shader;
	};
	bool aboveSelected = false;
	for (const Preset &preset :
		 {Preset{
			  "Weather Preset CLEAR",
			  4,
			  0,
			  0,
			  768,
			  384,
			  8,
			  20,
			  .005f,
			  0,
			  0,
			  .1f,
			  .34f,
			  .22f,
			  .35f,
			  CloudShader::Cumulus
		  },
		  Preset{
			  "Weather Preset SCATTERED CUMULUS",
			  6,
			  0,
			  0,
			  960,
			  640,
			  16,
			  16,
			  .009f,
			  0,
			  0,
			  1.8f,
			  .46f,
			  .54f,
			  .48f,
			  CloudShader::Cumulus
		  },
		  Preset{
			  "Weather Preset OVERCAST STRATUS",
			  13,
			  38,
			  .25f,
			  1024,
			  256,
			  32,
			  15,
			  .012f,
			  .04f,
			  .02f,
			  2,
			  .72f,
			  .74f,
			  .76f,
			  CloudShader::Stratus
		  },
		  Preset{
			  "Weather Preset TOWERING STORM",
			  24,
			  150,
			  1.4f,
			  896,
			  1024,
			  32,
			  13,
			  .016f,
			  .24f,
			  .3f,
			  5,
			  .96f,
			  .96f,
			  .96f,
			  CloudShader::Storm
		  },
		  Preset{
			  "Weather Preset WISPY ICE",
			  10,
			  8,
			  0,
			  2304,
			  512,
			  16,
			  -28,
			  .004f,
			  .96f,
			  0,
			  .2f,
			  .4f,
			  .38f,
			  .24f,
			  CloudShader::Stratus
		  },
		  Preset{
			  "Weather Preset GROUND FOG",
			  1.5f,
			  0,
			  0,
			  128,
			  256,
			  8,
			  17,
			  .014f,
			  0,
			  0,
			  0,
			  .14f,
			  .12f,
			  .1f,
			  CloudShader::Stratus
		  }}) {
		CAPTURE(preset.Button);
		const auto *previous = store.Get<engine::scene::Clouds>(provider);
		REQUIRE(previous);
		const auto previousClouds = *previous;
		ActivateControl(store, *runtime, preset.Button, .05);
		const auto *weather = store.Get<engine::scene::WeatherCompute>(provider);
		REQUIRE(weather);
		CHECK(weather->WindMetresPerSecond.Magnitude() == Catch::Approx(preset.Wind));
		CHECK(weather->WindMetresPerSecond.Y == 0);
		CHECK(weather->PrecipitationRate == Catch::Approx(preset.Precipitation));
		CHECK(weather->LightningRate == Catch::Approx(preset.Lightning));
		CHECK(weather->BaseHeightMetres == Catch::Approx(preset.BaseHeight));
		CHECK(weather->ThicknessMetres == Catch::Approx(preset.Thickness));
		CHECK(weather->FeatureCells == preset.FeatureCells);
		CHECK(weather->BaseTemperatureCelsius == Catch::Approx(preset.Temperature));
		CHECK(weather->TemperatureLapseCelsiusPerKilometre == Catch::Approx(6.5f));
		CHECK(weather->VapourMixingRatio == Catch::Approx(preset.Vapour));
		CHECK(weather->IceFraction == Catch::Approx(preset.Ice));
		CHECK(weather->HailIntensity == Catch::Approx(preset.Hail));
		CHECK(weather->LiftMetresPerSecond == Catch::Approx(preset.Lift));
		CHECK(weather->Coverage == Catch::Approx(preset.Coverage));
		CHECK((weather->ExtentMetres == engine::core::Vector3{2048, 1024, 2048}));
		const auto compiled = engine::weather::Compile(*weather);
		REQUIRE(compiled);
		const auto time =
			engine::weather::ResolveTime(engine::weather::InitialAnchor(compiled->Definition, 0), 0);
		REQUIRE(time);
		const auto sample = engine::weather::SampleConsumers(*compiled, *time, *weather, {0, 2, 0}, 0);
		REQUIRE(sample);
		const auto &precipitation = sample->Precipitation;
		CHECK(
			precipitation.LiquidMillimetresPerHour + precipitation.SnowMillimetresPerHour +
				precipitation.HailMillimetresPerHour ==
			Catch::Approx(preset.Precipitation)
		);
		if (std::string_view(preset.Button) == "Weather Preset WISPY ICE") {
			CHECK(precipitation.SnowMillimetresPerHour > 0);
			CHECK(precipitation.LiquidMillimetresPerHour == 0);
			CHECK(precipitation.HailMillimetresPerHour == 0);
		}
		if (std::string_view(preset.Button) == "Weather Preset GROUND FOG")
			CHECK(sample->FogExtinctionPerMetre > 0);
		const auto *shape = store.Get<engine::scene::CloudCompute>(provider);
		REQUIRE(shape);
		CHECK(shape->Shader == preset.Shader);
		const auto *clouds = store.Get<engine::scene::Clouds>(provider);
		REQUIRE(clouds);
		const auto approaches = [](float previous, float actual, float target) {
			if (std::abs(previous - target) < 1e-6f)
				CHECK(actual == Catch::Approx(target));
			else
				CHECK(std::abs(actual - target) < std::abs(previous - target));
		};
		approaches(previousClouds.Cover, clouds->Cover, preset.Cover);
		approaches(previousClouds.Density, clouds->Density, preset.Density);

		const float centre = preset.BaseHeight + preset.Thickness * .5f;
		const float top = preset.BaseHeight + preset.Thickness;
		if (aboveSelected) CHECK(CameraFrame(store, camera).Position.Y == Catch::Approx(top + 500));
		ActivateControl(store, *runtime, "Weather Scene Inside Clouds");
		const auto inside = CameraFrame(store, camera);
		CHECK(inside.Position.Y == Catch::Approx(centre));
		CHECK(inside.Position.Y > preset.BaseHeight);
		CHECK(inside.Position.Y < top);
		CHECK(inside.Position.X == 0);
		CHECK(inside.Position.Z == 700);
		CHECK(inside.LookVector().Y == Catch::Approx(0));
		ActivateControl(store, *runtime, "Weather Scene Above Clouds");
		const auto above = CameraFrame(store, camera);
		CHECK(above.Position.Y == Catch::Approx(top + 500));
		CHECK(above.LookVector().Y < 0);
		CHECK((above.Position - engine::core::Vector3{0, centre, 0}).Magnitude() < farPlane.Double);
		aboveSelected = true;
	}
	const auto modeButton = NamedInstance(store, "Weather Stateful Checkpoint");
	REQUIRE(modeButton != engine::ecs::NULL_ENTITY);
	for (const auto expected :
		 {engine::scene::WeatherMode::Stateful, engine::scene::WeatherMode::Procedural}) {
		ActivateControl(store, *runtime, "Weather Stateful Checkpoint");
		CHECK(store.Get<engine::scene::WeatherCompute>(provider)->Mode == expected);
	}
	ActivateControl(store, *runtime, "Weather Scene Roof");
	CHECK(store.Get<engine::scene::WeatherCompute>(provider)->PrecipitationRate == Catch::Approx(38));
	CHECK(CameraFrame(store, camera).Position.Y == Catch::Approx(9));
	ActivateControl(store, *runtime, "Weather Scene Storm");
	CHECK(store.Get<engine::scene::WeatherCompute>(provider)->StormIntensity == Catch::Approx(.88f));
	CHECK(CameraFrame(store, camera).Position.Y == Catch::Approx(18));
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
	CHECK(lighting.EnvironmentState.Weather.Mode == engine::scene::WeatherMode::Procedural);
	CHECK(lighting.EnvironmentState.Weather.LayerName == engine::core::Name("weather-demo"));
	CHECK(lighting.EnvironmentState.Weather.CellSizeMetres == Catch::Approx(64.0f));
	CHECK(lighting.EnvironmentState.Weather.BaseHeightMetres == Catch::Approx(1024.0f));
	CHECK(lighting.EnvironmentState.Weather.ThicknessMetres == Catch::Approx(256.0f));
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
		CHECK(weather.Mode == engine::scene::WeatherMode::Procedural);
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
