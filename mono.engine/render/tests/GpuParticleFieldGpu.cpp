// Real-device execution coverage for the analytical cosmetic field.

#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

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
		view.CameraFrame = core::CFrame::LookAt({0.0f, 185.0f, 650.0f}, {0.0f, 185.0f, 0.0f});
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

TEST_CASE(
	"GPU particle field dispatches and resizes without a CPU particle readback", "[render][gpu][particles]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");

	// The field is a broad atmospheric volume. A 960x540 target gives its
	// three-metre cloud billboards enough LDR coverage for a meaningful capture.
	render::SceneTarget target{960, 540};
	render::OverlayImage overlay;
	auto baseline = FieldView(target, 262'144, 17);
	baseline.GpuParticles.reset();
	fixture.Render.Render(std::span(&baseline, 1), overlay, nullptr, false);
	const CapturedImage empty = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		baseline.Slot,
		target.Width,
		target.Height,
		ImageFormat::Rgba8Unorm
	);
	auto first = FieldView(target, 262'144, 17);
	const render::FrameResult firstFrame =
		fixture.Render.Render(std::span(&first, 1), overlay, nullptr, false);
	CHECK(firstFrame.ComputeDispatches >= 1);
	CHECK(firstFrame.ParticlesDrawn >= 262'144);
	const CapturedImage field = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		first.Slot,
		target.Width,
		target.Height,
		ImageFormat::Rgba8Unorm
	);
	size_t changedBytes = 0;
	for (size_t index = 0; index < field.Bytes.size(); ++index) {
		changedBytes += field.Bytes[index] != empty.Bytes[index];
	}
	// This is the post-transparent LDR result, so a difference proves the field
	// affected rendered pixels rather than only recording a dispatch and draw.
	CHECK(changedBytes > 64);
	const render::GpuMemoryStatistics firstMemory = fixture.Render.MemoryStatistics();
	// The allocation path must retain the working 262k field when a larger
	// optional preset cannot be admitted.
	render::test_support::SetForceGpuParticleFieldAllocationFailure(true);
	auto refused = FieldView(target, 1'048'576, 19);
	const render::FrameResult refusedFrame =
		fixture.Render.Render(std::span(&refused, 1), overlay, nullptr, false);
	CHECK(refusedFrame.ParticlesDrawn >= 262'144);
	CHECK(refusedFrame.ParticlesDrawn < 1'048'576);
	CHECK(fixture.Render.MemoryStatistics().BufferBytes == firstMemory.BufferBytes);

	auto resized = FieldView(target, 1'048'576, 23);
	const render::FrameResult resizedFrame =
		fixture.Render.Render(std::span(&resized, 1), overlay, nullptr, false);
	CHECK(resizedFrame.ComputeDispatches >= 1);
	CHECK(resizedFrame.ParticlesDrawn >= 1'048'576);
	const render::GpuMemoryStatistics resizedMemory = fixture.Render.MemoryStatistics();
	// The simulation state is the only field allocation: 32 bytes per row. A
	// resize has no upload or download companion, so the transfer residency does
	// not grow with the particle count.
	CHECK(resizedMemory.BufferBytes >= firstMemory.BufferBytes + (1'048'576u - 262'144u) * 32ull);
	CHECK(resizedMemory.TransferBufferBytes == firstMemory.TransferBufferBytes);
}
