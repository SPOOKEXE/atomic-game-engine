#include "RenderFixture.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WeatherEffects.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

TEST_SUITE_ID("engine.render.weather-effects-gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.weather-effects")
TEST_DEPENDS("engine.weather.runtime")

using namespace engine;

TEST_CASE("weather precipitation composes with authored GPU fields and freezes", "[weather][gpu][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::SceneTarget target{129, 97};
	render::View view;
	view.Target = &target;
	view.World = 831;
	view.WorldName = core::Name("weather.particle.composition");
	view.CameraFrame = core::CFrame::LookAt({0, 20, 130}, {0, 20, 0});
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1, 1, 1};
	view.ParticleDelta = 0;
	render::OverlayImage overlay;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto empty = render::test::CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		view.Slot,
		target.Width,
		target.Height,
		render::test::ImageFormat::Bgra8Unorm
	);
	const auto population = [](float horizontal, core::Color3 colour, uint64_t source) {
		render::GpuParticleFieldView packet;
		packet.Source = ecs::Entity{source};
		packet.DefinitionRevision = 1;
		packet.Field.RequestedCount = 262144;
		packet.Field.Layers = 1;
		packet.Field.HalfExtent = {64, 64, 64};
		packet.Field.VelocityResponse = 0;
		packet.Field.Styles[0].Colour = colour;
		packet.Field.Styles[0].Alpha = 0.02f;
		packet.Field.Styles[0].Size = 1.5f;
		for (uint32_t index = 0; index < 96; ++index) {
			packet.Field.SpawnSamples.push_back(
				{{horizontal + float(index % 8) * 2, 8 + float(index / 8) * 2, 0}, 2, {0, -9, 0}, 0}
			);
		}
		return packet;
	};
	view.GpuParticles = population(-36, {1, 0, 0}, 5);
	const auto rain = population(18, {0, 0, 1}, 6);
	view.WeatherParticles = std::span<const render::GpuParticleFieldView>(&rain, 1);
	view.Lighting.EnvironmentState.Clock.WorldPaused = true;
	const auto emitted = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(emitted.Submitted);
	CHECK(emitted.Particles == 524288);
	CHECK(emitted.ComputeDispatches >= 2);
	const auto combined = render::test::CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		view.Slot,
		target.Width,
		target.Height,
		render::test::ImageFormat::Bgra8Unorm
	);
	size_t red = 0, blue = 0;
	for (uint32_t y = 0; y < combined.Height; ++y) {
		for (uint32_t x = 0; x < combined.Width; ++x) {
			const auto offset = y * combined.RowStrideBytes + x * 4;
			if (combined.Bytes[offset] == empty.Bytes[offset] &&
				combined.Bytes[offset + 1] == empty.Bytes[offset + 1] &&
				combined.Bytes[offset + 2] == empty.Bytes[offset + 2])
				continue;
			const int b = std::to_integer<int>(combined.Bytes[offset]),
					  g = std::to_integer<int>(combined.Bytes[offset + 1]),
					  r = std::to_integer<int>(combined.Bytes[offset + 2]);
			red += r > b && r > g;
			blue += b > r && b > g;
		}
	}
	CHECK(red > 8);
	CHECK(blue > 8);
	const auto paused = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(paused.Submitted);
	CHECK(paused.ComputeDispatches == 0);
	const auto repeated = render::test::CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		view.Slot,
		target.Width,
		target.Height,
		render::test::ImageFormat::Bgra8Unorm
	);
	CHECK(repeated.Bytes == combined.Bytes);
	fixture.Render.ForgetWorld(view.World, view.WorldName);
}

TEST_CASE("authoritative weather lightning renders distinct branches and expires", "[weather][gpu][.]") {
	ecs::Store store("weather.lightning.image");
	scene::RegisterSceneClasses();
	scene::RegisterWeatherClockComponents();
	weather::RegisterWeatherRuntimeComponents();
	const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Storm");
	REQUIRE(provider != ecs::NULL_ENTITY);
	scene::WeatherCompute controls;
	controls.Seed = 24;
	controls.TickRate = 1;
	controls.BaseHeightMetres = 20;
	controls.ThicknessMetres = 120;
	controls.BaseTemperatureCelsius = -20;
	controls.TemperatureVariationCelsius = 0;
	controls.TemperatureLapseCelsiusPerKilometre = 0;
	controls.IceFraction = 1;
	controls.StormIntensity = 1;
	controls.LightningRate = 1;
	controls.WindMetresPerSecond = {0, 8, 0};
	controls.LiftMetresPerSecond = 0;
	controls.TurbulenceMetresPerSecond = 0;
	controls.ShearMetresPerSecond = 0;
	store.Set(provider, controls);
	store.SetResource(weather::WeatherGroundQuery{[](const ecs::Store &, core::Vector3, float) {
		return std::optional(0.0f);
	}});
	store.AdvanceTick(1.0f);
	weather::UpdateWeatherClocks(store);
	weather::UpdateWeatherRuntime(store);
	const auto *journal = store.Get<weather::WeatherEvents>(provider);
	REQUIRE(journal);
	REQUIRE(journal->Events.size() == 1);
	const auto event = journal->Events.front();
	REQUIRE(event.Kind == weather::StormEventKind::Lightning);
	REQUIRE(event.Tick == 1);
	REQUIRE(event.Id.Seed == 24);
	REQUIRE(event.StartMetres.Y == 110);
	REQUIRE(event.EndMetres.Y == 0);

	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::SceneTarget target{513, 385};
	render::View view;
	view.Target = &target;
	view.World = store.Identity();
	view.WorldName = core::Name(store.Name());
	const auto centre = (event.StartMetres + event.EndMetres) * 0.5f;
	view.CameraFrame = core::CFrame::LookAt(centre + core::Vector3{0, 0, 160}, centre);
	view.Camera.FieldOfViewRadians = 0.9f;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {0, 0, 0};
	view.ParticleDelta = 0;
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasWeatherCompute = true;
	environment.HasWeatherClock = true;
	environment.CloudProvider = provider;
	environment.Weather = *scene::WeatherControlsOf(store, provider);
	environment.Clock = *store.Get<scene::WeatherClock>(provider);
	render::OverlayImage overlay;
	const auto capture = [&] {
		REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
		return render::test::CaptureResource(
			fixture.Render,
			core::Name("composed-image"),
			view.Slot,
			target.Width,
			target.Height,
			render::test::ImageFormat::Bgra8Unorm
		);
	};
	const auto before = capture();
	render::WeatherEffectsFrame frame;
	effects::RibbonBuffer ribbons;
	std::vector<render::SceneLight> lights;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(ribbons.Runs.size() == weather::MAX_LIGHTNING_SEGMENTS);
	REQUIRE(lights.size() == 1);
	view.RibbonVertices = ribbons.Vertices;
	view.RibbonRuns = ribbons.Runs;
	view.Lights = lights;
	const auto discharge = capture();

	// Branch pixels must lie away from every main-channel segment. A bright central stripe
	// alone cannot satisfy this image check, even if all twenty-eight runs were submitted.
	std::array<weather::LightningSegment, weather::MAX_LIGHTNING_SEGMENTS> segments;
	REQUIRE(weather::BuildLightningSegments(event, segments) == segments.size());
	const auto matrices =
		scene::ResolveCamera(view.CameraFrame, view.Camera, float(target.Width) / float(target.Height));
	const auto project = [&](core::Vector3 point) {
		const auto clip = matrices.ViewProjection * glm::vec4(point.X, point.Y, point.Z, 1);
		return core::Vector2{
			(clip.x / clip.w * 0.5f + 0.5f) * target.Width, (0.5f - clip.y / clip.w * 0.5f) * target.Height
		};
	};
	std::array<std::array<core::Vector2, 2>, weather::MAX_LIGHTNING_SEGMENTS> projected;
	for (size_t index = 0; index < segments.size(); ++index)
		projected[index] = {project(segments[index].StartMetres), project(segments[index].EndMetres)};
	const auto distance = [](core::Vector2 pixel, const std::array<core::Vector2, 2> &line) {
		const auto along = line[1] - line[0];
		const float lengthSquared = along.MagnitudeSquared();
		const float fraction =
			lengthSquared > 0 ? std::clamp((pixel - line[0]).Dot(along) / lengthSquared, 0.0f, 1.0f) : 0;
		return (pixel - line[0] - along * fraction).Magnitude();
	};
	size_t mainPixels = 0;
	std::array<size_t, 4> branchPixels{};
	for (uint32_t y = 0; y < target.Height; ++y) {
		for (uint32_t x = 0; x < target.Width; ++x) {
			const size_t offset = y * discharge.RowStrideBytes + x * 4;
			bool brighter = false;
			for (size_t channel = 0; channel < 3; ++channel)
				brighter |= std::to_integer<int>(discharge.Bytes[offset + channel]) >
							std::to_integer<int>(before.Bytes[offset + channel]) + 12;
			if (!brighter) continue;
			const core::Vector2 pixel{float(x) + 0.5f, float(y) + 0.5f};
			float mainDistance = INFINITY;
			for (size_t index = 0; index < segments.size(); ++index)
				if (segments[index].Branch == 0)
					mainDistance = std::min(mainDistance, distance(pixel, projected[index]));
			mainPixels += mainDistance < 2;
			if (mainDistance <= 3) continue;
			std::array<bool, 4> branchHits{};
			for (size_t index = 0; index < segments.size(); ++index)
				if (segments[index].Branch != 0 && distance(pixel, projected[index]) < 2)
					branchHits[segments[index].Branch - 1] = true;
			for (size_t branch = 0; branch < branchHits.size(); ++branch)
				branchPixels[branch] += branchHits[branch];
		}
	}
	INFO("main channel pixels: " << mainPixels);
	INFO(
		"branch pixels: " << branchPixels[0] << ',' << branchPixels[1] << ',' << branchPixels[2] << ','
						  << branchPixels[3]
	);
	CHECK(mainPixels > 100);
	CHECK(std::count_if(branchPixels.begin(), branchPixels.end(), [](size_t count) {
			  return count >= 3;
		  }) >= 2);

	ribbons = {};
	lights.clear();
	environment.Clock.ElapsedFractionQ32 = uint32_t{1} << 31;
	render::CollectWeatherEffects(store, view, frame, ribbons, lights);
	REQUIRE(ribbons.Runs.empty());
	REQUIRE(lights.empty());
	view.RibbonVertices = ribbons.Vertices;
	view.RibbonRuns = ribbons.Runs;
	view.Lights = lights;
	const auto expired = capture();
	CHECK(expired.Bytes == before.Bytes);
	fixture.Render.ForgetWorld(view.World, view.WorldName);
}
