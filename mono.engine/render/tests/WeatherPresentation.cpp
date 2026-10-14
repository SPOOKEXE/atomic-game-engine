// Weather cache keys follow semantic inputs and accepted integral time without a device.

#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.weatherpresentation")

using namespace engine;

namespace {
	render::ScenePresentationState ActiveWeather() {
		render::ScenePresentationState state;
		auto &environment = state.Lighting.EnvironmentState;
		environment.HasClouds = true;
		environment.HasCloudCompute = true;
		environment.HasWeatherCompute = true;
		environment.HasWeatherClock = true;
		environment.CloudProvider = ecs::Entity{17};
		environment.CloudLayer.WindSpeed = 0;
		environment.Clock.Tick = (uint64_t{1} << 54) + 3;
		environment.Clock.AnchorTick = environment.Clock.Tick;
		environment.Clock.PhaseTick = 100;
		environment.Clock.WindQ20 = {8 * 4096, 0, 0};
		return state;
	}

	uint64_t Signature(const render::ScenePresentationState &state) {
		return render::ScenePresentationSignaturesOf({}, state).Environment;
	}
}

TEST_CASE(
	"weather cache notices canonical ticks without reacting to receipt fractions", "[render][weather][cache]"
) {
	auto state = ActiveWeather();
	const auto original = Signature(state);
	REQUIRE(original != 0);
	auto &environment = state.Lighting.EnvironmentState;
	environment.Clock.Fraction = 0.75;
	environment.Clock.Sequence += 10;
	CHECK(Signature(state) == original);
	environment.CloudTime = 4000;
	CHECK(Signature(state) == original);
	environment.Clock.Tick++;
	CHECK(Signature(state) != original);
	state = ActiveWeather();
	environment.Clock.PhaseTick++;
	CHECK(Signature(state) != original);
	state = ActiveWeather();
	environment.Clock.DisplacementQ8[1]++;
	CHECK(Signature(state) != original);
	state = ActiveWeather();
	environment.Clock.DefinitionRevision++;
	CHECK(Signature(state) != original);
	state = ActiveWeather();
	environment.WeatherHistoryRevision++;
	CHECK(Signature(state) != original);
	state = ActiveWeather();
	environment.Clock.Epoch++;
	CHECK(Signature(state) != original);
}

TEST_CASE(
	"raw weather recipe edits invalidate the cloud image without a revision bump", "[render][weather][cache]"
) {
	const auto initial = ActiveWeather();
	const auto original = Signature(initial);
	const std::array fields{
		&scene::WeatherCompute::CellSizeMetres,
		&scene::WeatherCompute::BaseHeightMetres,
		&scene::WeatherCompute::ThicknessMetres,
		&scene::WeatherCompute::CondensateScale,
		&scene::WeatherCompute::VapourMixingRatio,
		&scene::WeatherCompute::BaseTemperatureCelsius,
		&scene::WeatherCompute::FogStrength,
		&scene::WeatherCompute::PrecipitationRate,
		&scene::WeatherCompute::StormIntensity,
	};
	for (const auto member : fields) {
		auto changed = initial;
		changed.Lighting.EnvironmentState.Weather.*member += 0.125f;
		CHECK(Signature(changed) != original);
	}
	auto changed = initial;
	changed.Lighting.EnvironmentState.Weather.Seed++;
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.Weather.ExtentMetres.X++;
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.Weather.LayerName = core::Name("high-ice");
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.CloudProvider = ecs::Entity{18};
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.CloudVolume.Seed++;
	CHECK(Signature(changed) == original);
}

TEST_CASE(
	"stateful boundary and surface edits invalidate unchanged reflection revisions",
	"[render][weather][cache]"
) {
	auto initial = ActiveWeather();
	initial.Lighting.EnvironmentState.Weather.Mode = scene::WeatherMode::Stateful;
	const auto original = Signature(initial);
	for (const auto member :
		 {&scene::WeatherCompute::GroundVapourFluxKilogramsPerSquareMetreSecond,
		  &scene::WeatherCompute::RainFallSpeedMetresPerSecond}) {
		auto changed = initial;
		changed.Lighting.EnvironmentState.Weather.*member += 0.125f;
		CHECK(Signature(changed) != original);
	}
	auto changed = initial;
	changed.Lighting.EnvironmentState.Weather.BoundaryX = scene::WeatherBoundary::Closed;
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.Weather.BoundaryZ = scene::WeatherBoundary::Closed;
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.Weather.TopBoundary = scene::WeatherTopBoundary::Escape;
	CHECK(Signature(changed) != original);
	changed = initial;
	changed.Lighting.EnvironmentState.Weather.TerrainRevision++;
	CHECK(Signature(changed) != original);
	render::View view;
	const auto terrain = render::ScenePresentationSignaturesOf(view, initial).Environment;
	view.WeatherTerrainRevision++;
	CHECK(render::ScenePresentationSignaturesOf(view, initial).Environment != terrain);
}

TEST_CASE("disabled weather controls do not invalidate the remaining sky", "[render][weather][cache]") {
	auto state = ActiveWeather();
	auto &environment = state.Lighting.EnvironmentState;
	environment.Skybox = scene::SkyboxSource::Compute;
	environment.Weather.Enabled = false;
	const auto original = Signature(state);
	REQUIRE(original != 0);
	environment.Weather.Seed++;
	environment.Weather.Coverage = 0;
	environment.Weather.CondensateScale *= 2;
	environment.Weather.LayerName = core::Name("unused-weather");
	environment.Weather.DefinitionRevision++;
	environment.Clock.Tick++;
	environment.Clock.Epoch++;
	environment.WeatherHistoryRevision++;
	CHECK(Signature(state) == original);
	environment.Skybox = scene::SkyboxSource::None;
	CHECK_FALSE(render::EnvironmentLayerPresent(state.Lighting));
	CHECK(Signature(state) == 0);

	state = ActiveWeather();
	environment.Weather.ShadowsEnabled = false;
	const auto unshadowed = Signature(state);
	environment.Weather.ShadowStrength = 0;
	environment.Weather.ShadowResolution *= 2;
	CHECK(Signature(state) == unshadowed);
}

TEST_CASE(
	"paused weather reuses its image while its own wind and camera still invalidate",
	"[render][weather][cache]"
) {
	auto state = ActiveWeather();
	auto &environment = state.Lighting.EnvironmentState;
	environment.Clock.Paused = true;
	environment.Weather.Paused = true;
	environment.Weather.WindMetresPerSecond = {};
	environment.Weather.GrowthRate = 0;
	const auto paused = Signature(state);
	environment.Clock.Tick += 500;
	environment.Clock.Fraction = 0.5;
	environment.Clock.Sequence++;
	CHECK(Signature(state) == paused);
	environment.Weather.WindMetresPerSecond.X = 12;
	CHECK(Signature(state) != paused);

	state = ActiveWeather();
	const auto moving = Signature(state);
	CHECK(environment.CloudLayer.WindSpeed == 0);
	environment.Clock.Tick++;
	CHECK(Signature(state) != moving);
	render::View view;
	const auto beforeCamera = render::ScenePresentationSignaturesOf(view, state);
	view.CameraFrame.Position.Y = 1500;
	const auto afterCamera = render::ScenePresentationSignaturesOf(view, state);
	CHECK(afterCamera.Environment != beforeCamera.Environment);
	CHECK(afterCamera.Objects == beforeCamera.Objects);
}

TEST_CASE(
	"separate weather particle sources invalidate only the particle cache", "[render][weather][cache]"
) {
	render::View view;
	const auto empty = render::ScenePresentationSignaturesOf(view, {});
	render::GpuParticleFieldView packet;
	packet.DefinitionRevision = 1;
	view.WeatherParticles = std::span<const render::GpuParticleFieldView>(&packet, 1);
	const auto weatherParticles = render::ScenePresentationSignaturesOf(view, {});
	CHECK(weatherParticles.Particles != empty.Particles);
	CHECK(weatherParticles.Environment == empty.Environment);
	CHECK(weatherParticles.Objects == empty.Objects);
	const auto visibility = render::ParticleVisibilitySignature(view);
	packet.Field.Seed++;
	CHECK(render::ParticleVisibilitySignature(view) != visibility);
	CHECK(render::ScenePresentationSignaturesOf(view, {}).Particles != weatherParticles.Particles);
	view.GpuParticles.emplace();
	view.GpuParticles->DefinitionRevision = 1;
	const auto combined = render::ScenePresentationSignaturesOf(view, {}).Particles;
	view.GpuParticles->Field.Seed++;
	CHECK(render::ScenePresentationSignaturesOf(view, {}).Particles != combined);
}

TEST_CASE("hidden clouds retain independently visible shadow source keys", "[render][weather][cache]") {
	auto state = ActiveWeather();
	auto &environment = state.Lighting.EnvironmentState;
	environment.CloudLayer.Enabled = false;
	REQUIRE(render::EnvironmentLayerPresent(state.Lighting));
	const auto original = Signature(state);
	REQUIRE(original != 0);
	environment.CloudLayer.Colour.R = 0.1f;
	CHECK(Signature(state) == original);
	const auto originalCover = environment.CloudLayer.Cover;
	environment.CloudLayer.Cover = 0.1f;
	CHECK(Signature(state) != original);
	environment.CloudLayer.Cover = originalCover;
	environment.Weather.Seed++;
	CHECK(Signature(state) != original);
	environment.Weather.Seed--;
	environment.Clock.Tick++;
	CHECK(Signature(state) != original);
	environment.Clock.Tick--;
	state.Lighting.Direction = {0.1f, -0.8f, 0.2f};
	CHECK(Signature(state) != original);
	render::View camera;
	const auto beforeCamera = render::ScenePresentationSignaturesOf(camera, state);
	camera.CameraFrame.Position.X = 64;
	CHECK(render::ScenePresentationSignaturesOf(camera, state).Environment != beforeCamera.Environment);
	environment.Weather.ShadowsEnabled = false;
	CHECK_FALSE(render::EnvironmentLayerPresent(state.Lighting));
	CHECK(Signature(state) == 0);
}
