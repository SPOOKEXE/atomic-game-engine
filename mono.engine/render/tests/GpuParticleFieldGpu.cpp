// Real-device execution coverage for the analytical cosmetic field.

#include "RenderFixture.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.gpuparticlefieldgpu")
TEST_DEPENDS("engine.render.fixtures")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	render::View FieldView(render::SceneTarget &target, uint32_t count, uint32_t seed) {
		render::View view;
		view.Target = &target;
		view.World = 71;
		view.WorldName = core::Name("gpu-particle-field-test");
		view.CameraFrame = core::CFrame({0.0f, 120.0f, 600.0f});
		view.Camera.FarPlane = 4'000.0f;
		view.ParticleDelta = 1.0f / 60.0f;
		view.GpuParticles = render::GpuParticleFieldView{
			.Field = {.RequestedCount = count, .Seed = seed},
			.Storm = scene::EfPreset(scene::EfCategory::EF3),
			.Centre = {},
			.Seconds = 1.0f,
		};
		return view;
	}
}

TEST_CASE("GPU particle field dispatches and resizes without a CPU particle readback", "[render][gpu][particles]") {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");

	render::SceneTarget target{96, 64};
	auto first = FieldView(target, 262'144, 17);
	const render::FrameResult firstFrame = fixture.Render.Render(std::span(&first, 1), nullptr, nullptr, false);
	CHECK(firstFrame.ComputeDispatches >= 1);
	CHECK(firstFrame.ParticlesDrawn >= 262'144);

	auto resized = FieldView(target, 1'048'576, 23);
	const render::FrameResult resizedFrame = fixture.Render.Render(std::span(&resized, 1), nullptr, nullptr, false);
	CHECK(resizedFrame.ComputeDispatches >= 1);
	CHECK(resizedFrame.ParticlesDrawn >= 1'048'576);
}
