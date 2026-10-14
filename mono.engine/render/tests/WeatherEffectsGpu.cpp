#include "RenderFixture.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.weather-effects-gpu")
TEST_DEPENDS("engine.render.fixtures")

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
