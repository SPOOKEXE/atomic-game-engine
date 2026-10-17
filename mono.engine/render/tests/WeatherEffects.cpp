#include <engine/ecs/Store.hpp>
#include <engine/render/WeatherEffects.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.weather-effects")
TEST_DEPENDS("engine.weather.consumers")
TEST_DEPENDS("engine.weather.events")

using namespace engine;
using Catch::Approx;

namespace {
	ecs::Entity Provider(ecs::Store &store) {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		const auto provider =
			store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Weather");
		REQUIRE(provider != ecs::NULL_ENTITY);
		auto controls = *store.Get<scene::WeatherCompute>(provider);
		controls.PrecipitationRate = 8;
		controls.TemperatureVariationCelsius = 0;
		controls.TemperatureLapseCelsiusPerKilometre = 0;
		controls.BaseTemperatureCelsius = 15;
		store.Set(provider, controls);
		store.Set(provider, scene::WeatherClock{});
		return provider;
	}
	render::View ViewOf(ecs::Store &store, ecs::Entity provider) {
		render::View view;
		view.World = store.Identity();
		view.WorldName = core::Name(store.Name());
		view.CameraFrame.Position = {0, 2, 0};
		auto &environment = view.Lighting.EnvironmentState;
		environment.HasWeatherCompute = true;
		environment.HasWeatherClock = true;
		environment.CloudProvider = provider;
		environment.Weather = *store.Get<scene::WeatherCompute>(provider);
		environment.Clock = *store.Get<scene::WeatherClock>(provider);
		return view;
	}
}

TEST_CASE("weather geometry clips trajectories at the nearest actual oriented bound", "[weather]") {
	std::array<scene::DrawInstance, 2> geometry;
	geometry[0].Frame.Position = {0, 5, 0};
	geometry[0].HalfExtent = {4, 1, 4};
	geometry[1].Frame.Position = {0, 1, 0};
	geometry[1].HalfExtent = {4, 1, 4};
	const auto hit = render::WeatherGeometryHit(geometry, {0, 10, 0}, {0, -2, 0}, 10);
	REQUIRE(hit);
	CHECK(*hit == Approx(2));
	CHECK_FALSE(render::WeatherGeometryHit(geometry, {8, 10, 0}, {0, -2, 0}, 10));
	geometry[0].Transparency = 0.75f;
	const auto lower = render::WeatherGeometryHit(geometry, {0, 10, 0}, {0, -2, 0}, 10);
	REQUIRE(lower);
	CHECK(*lower == Approx(4));
	CHECK_FALSE(render::WeatherGeometryHit(geometry, {}, {}, -1));
}

TEST_CASE("weather emits generic particles and preserves them across quiet maintenance", "[weather]") {
	ecs::Store store("weather.effects");
	const auto provider = Provider(store);
	auto view = ViewOf(store, provider);
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(frame.Precipitation);
	CHECK(frame.Precipitation->Source == provider);
	CHECK(scene::ValidGpuParticleField(frame.Precipitation->Field));
	const auto *samples = frame.Precipitation->Field.SpawnSamples.data();
	const auto revision = frame.Precipitation->DefinitionRevision;
	view.Lighting.EnvironmentState.Clock.Tick = 1;
	view.Lighting.Ambient = {0.2f, 0.3f, 0.4f};
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(frame.Precipitation);
	CHECK(frame.Precipitation->Field.SpawnSamples.data() == samples);
	CHECK(frame.Precipitation->DefinitionRevision == revision);
	CHECK(frame.Lighting.Ambient == view.Lighting.Ambient);
	view.Lighting.EnvironmentState.Weather.Enabled = false;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK_FALSE(frame.Precipitation);
	CHECK_FALSE(frame.HasLighting);
}

TEST_CASE("a roof suppresses indoor precipitation without changing the authored rain rate", "[weather]") {
	ecs::Store store("weather.roof");
	const auto provider = Provider(store);
	auto view = ViewOf(store, provider);
	scene::DrawInstance roof;
	roof.Frame.Position = {0, 100, 0};
	roof.HalfExtent = {256, 1, 256};
	view.Instances = std::span<const scene::DrawInstance>(&roof, 1);
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK_FALSE(frame.Precipitation);
	const auto query = weather::QueryWeather(store, provider, view.CameraFrame.Position);
	REQUIRE(query);
	CHECK(query->Precipitation.LiquidMillimetresPerHour == Approx(8));
}

TEST_CASE("deterministic storm geometry reaches actual ribbons and expires by layer clock", "[weather]") {
	ecs::Store store("weather.flash");
	const auto provider = Provider(store);
	auto view = ViewOf(store, provider);
	weather::StormEvent event;
	event.Layer = weather::WeatherLayerName(store, provider);
	event.Id = {1, 1, 1};
	event.StartMetres = {0, 100, 0};
	event.EndMetres = {10, 0, 0};
	event.BranchSeed = 24;
	store.Set(provider, weather::WeatherEvents{{event}, 0});
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK(ribbons.Runs.size() == weather::MAX_LIGHTNING_SEGMENTS);
	CHECK(ribbons.Vertices.size() == weather::MAX_LIGHTNING_SEGMENTS * 4);
	REQUIRE(lights.size() == 1);
	CHECK(lights[0].Range == 512);
	ribbons = {};
	lights.clear();
	view.Lighting.EnvironmentState.Clock.Tick = 6;
	view.Lighting.EnvironmentState.Clock.ElapsedFractionQ32 =
		static_cast<uint32_t>(uint64_t{3} * (uint64_t{1} << 32) / 10);
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK(ribbons.Runs.empty());
	CHECK(lights.empty());
}

TEST_CASE("lightning expires by copied elapsed age across skipped base-rate edits", "[weather]") {
	ecs::Store store("weather.flash.rate");
	const auto provider = Provider(store);
	auto view = ViewOf(store, provider);
	weather::StormEvent event;
	event.Layer = weather::WeatherLayerName(store, provider);
	event.Id = {1, 1, 1};
	event.Tick = 1000;
	event.ElapsedSeconds = UINT64_MAX - 100;
	event.StartMetres = {0, 100, 0};
	event.EndMetres = {10, 0, 0};
	event.BranchSeed = 24;
	store.Set(provider, weather::WeatherEvents{{event}, 0});
	auto &clock = view.Lighting.EnvironmentState.Clock;
	clock.Tick = 1020;
	clock.TickRate = 100;
	clock.ElapsedSeconds = event.ElapsedSeconds;
	clock.ElapsedFractionQ32 = uint32_t{1} << 30;
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(ribbons.Runs.size() == weather::MAX_LIGHTNING_SEGMENTS);
	REQUIRE(lights.size() == 1);
	CHECK(lights.front().Colour.R == Approx(15.0f * (1.0f - 0.25f / 0.3f)));
	ribbons = {};
	lights.clear();
	clock.TickRate = 40;
	clock.Tick = 1024;
	clock.ElapsedFractionQ32 = uint32_t{3} << 29;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK(ribbons.Runs.empty());
	CHECK(lights.empty());
}

TEST_CASE("ground fog follows elevated terrain and composes once above the medium", "[weather]") {
	ecs::Store store("weather.fog.terrain");
	const auto provider = Provider(store);
	auto controls = *store.Get<scene::WeatherCompute>(provider);
	controls.FogStrength = 1;
	controls.BaseHeightMetres = 40;
	controls.BaseTemperatureCelsius = 5;
	controls.VapourMixingRatio = 0.03f;
	controls.AtmosphereDensityStrength = 0.5f;
	controls.AtmosphereHazeStrength = 0.5f;
	++controls.DefinitionRevision;
	store.Set(provider, controls);
	auto clock = *store.Get<scene::WeatherClock>(provider);
	clock.DefinitionRevision = controls.DefinitionRevision;
	store.Set(provider, clock);
	auto view = ViewOf(store, provider);
	view.CameraFrame.Position.Y = 100;
	view.Lighting.VolumeCount = 1;
	view.Lighting.Volumes[0].Enabled = true;
	view.Lighting.EnvironmentState.Air.Colour = {0.3f, 0.4f, 0.5f};
	scene::DrawInstance ground;
	ground.Frame.Position = {0, 25, 0};
	ground.HalfExtent = {512, 1, 512};
	view.Instances = std::span<const scene::DrawInstance>(&ground, 1);
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(frame.Fog);
	CHECK(frame.Fog->Frame.Position.Y - frame.Fog->HalfExtent.Y == Approx(26));
	CHECK(frame.Fog->Frame.Position.Y + frame.Fog->HalfExtent.Y == Approx(40));
	CHECK(frame.Fog->Extinction > 0);
	CHECK(frame.Lighting.VolumeCount == 2);
	CHECK(frame.Lighting.EnvironmentState.Air.Colour == view.Lighting.EnvironmentState.Air.Colour);
	CHECK(frame.Lighting.EnvironmentState.Air.Density > view.Lighting.EnvironmentState.Air.Density);
	CHECK(frame.Lighting.EnvironmentState.Air.Haze == view.Lighting.EnvironmentState.Air.Haze);
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	CHECK(frame.Lighting.VolumeCount == 2);
}
