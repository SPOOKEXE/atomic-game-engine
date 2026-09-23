// Real-device execution coverage for the analytical cosmetic field.

#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

#include <engine/render/Renderer.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

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
		view.Lighting.Ambient = {1.0f, 1.0f, 1.0f};
		view.Lighting.FogEnd = 100'000.0f;
		view.ParticleDelta = 1.0f / 60.0f;
		view.GpuParticles = render::GpuParticleFieldView{
			.Field = {.RequestedCount = count, .Seed = seed},
			.Storm = scene::EfPreset(scene::EfCategory::EF3),
			.Centre = {},
			.Seconds = 1.0f,
		};
		return view;
	}

	size_t ChangedBytes(const CapturedImage &expected, const CapturedImage &actual) {
		REQUIRE(expected.Bytes.size() == actual.Bytes.size());
		size_t changed = 0;
		for (size_t index = 0; index < actual.Bytes.size(); ++index) {
			changed += actual.Bytes[index] != expected.Bytes[index];
		}
		return changed;
	}

	size_t RedDominantPixels(const CapturedImage &image, const CapturedImage &empty) {
		size_t pixels = 0;
		// The composed swapchain image stores blue before red in each raw pixel.
		REQUIRE(image.Format == ImageFormat::Bgra8Unorm);
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto channel = [&](size_t index) {
					return std::to_integer<int>(image.Bytes[offset + index]);
				};
				if (image.Bytes[offset] == empty.Bytes[offset] &&
					image.Bytes[offset + 1] == empty.Bytes[offset + 1] &&
					image.Bytes[offset + 2] == empty.Bytes[offset + 2])
					continue;
				pixels += channel(2) > channel(1) && channel(2) > channel(0);
			}
		}
		return pixels;
	}

	struct PixelBounds {
		uint32_t Left = 0;
		uint32_t Top = 0;
		uint32_t Right = 0;
		uint32_t Bottom = 0;
		bool Found = false;

		uint32_t Width() const {
			return Found ? Right - Left + 1 : 0;
		}
		uint32_t Height() const {
			return Found ? Bottom - Top + 1 : 0;
		}
	};

	PixelBounds RedParticleBounds(const CapturedImage &image, const CapturedImage &empty) {
		REQUIRE(image.Format == ImageFormat::Bgra8Unorm);
		PixelBounds bounds;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto channel = [&](size_t index) {
					return std::to_integer<int>(image.Bytes[offset + index]);
				};
				if ((image.Bytes[offset] == empty.Bytes[offset] &&
					 image.Bytes[offset + 1] == empty.Bytes[offset + 1] &&
					 image.Bytes[offset + 2] == empty.Bytes[offset + 2]) ||
					channel(2) <= channel(1) || channel(2) <= channel(0))
					continue;
				if (!bounds.Found) {
					bounds = {x, y, x, y, true};
				} else {
					bounds.Left = std::min(bounds.Left, x);
					bounds.Top = std::min(bounds.Top, y);
					bounds.Right = std::max(bounds.Right, x);
					bounds.Bottom = std::max(bounds.Bottom, y);
				}
			}
		}
		return bounds;
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
		ImageFormat::Bgra8Unorm
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
		ImageFormat::Bgra8Unorm
	);
	// This is the post-transparent LDR result, so a difference proves the field
	// affected rendered pixels rather than only recording a dispatch and draw.
	CHECK(ChangedBytes(empty, field) > 64);

	auto disabled = FieldView(target, 262'144, 17);
	disabled.GpuParticles->Field.Enabled = false;
	const render::FrameResult disabledFrame =
		fixture.Render.Render(std::span(&disabled, 1), overlay, nullptr, false);
	CHECK(disabledFrame.ParticlesDrawn == 0);
	const CapturedImage disabledImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		disabled.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	CHECK(ChangedBytes(empty, disabledImage) == 0);

	auto red = FieldView(target, 262'144, 17);
	red.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::Condensation);
	red.GpuParticles->Field.CondensationColor = {1.0f, 0.0f, 0.0f};
	red.GpuParticles->Field.CondensationAlpha = 0.8f;
	red.GpuParticles->Field.CondensationSize = 3.0f;
	const render::FrameResult redFrame = fixture.Render.Render(std::span(&red, 1), overlay, nullptr, false);
	CHECK(redFrame.ParticlesDrawn > 0);
	const CapturedImage redImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		red.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	CHECK(ChangedBytes(empty, redImage) > 64);
	CHECK(RedDominantPixels(redImage, empty) > 64);
	const PixelBounds condensation = RedParticleBounds(redImage, empty);
	// The red-only capture isolates the condensation lanes. Its tall, bounded
	// envelope proves parcels recycle below the zero-lift ceiling instead of
	// accumulating into an upper rectangular cap.
	CHECK(condensation.Height() > target.Height / 4);
	CHECK(condensation.Height() < target.Height * 3 / 8);
	CHECK(condensation.Width() < target.Width / 3);

	auto shortFunnel = FieldView(target, 262'144, 17);
	shortFunnel.World = 72;
	shortFunnel.WorldName = core::Name("gpu-particle-field-short-funnel-test");
	shortFunnel.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::Condensation);
	shortFunnel.GpuParticles->Field.CondensationColor = {1.0f, 0.0f, 0.0f};
	shortFunnel.GpuParticles->Field.CondensationAlpha = 0.8f;
	shortFunnel.GpuParticles->Field.CondensationSize = 3.0f;
	shortFunnel.GpuParticles->Storm.TopHeight = 120.0f;
	fixture.Render.Render(std::span(&shortFunnel, 1), overlay, nullptr, false);
	const CapturedImage shortFunnelImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		shortFunnel.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	// Condensation size and opacity are both normalized to storm height. A
	// shorter analytical field must therefore produce a distinct captured puff
	// envelope rather than reusing the tall funnel's fixed billboard pattern.
	CHECK(ChangedBytes(redImage, shortFunnelImage) > 512);

	auto smallerFainter = FieldView(target, 262'144, 17);
	smallerFainter.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::Condensation);
	smallerFainter.GpuParticles->Field.CondensationColor = {1.0f, 0.0f, 0.0f};
	smallerFainter.GpuParticles->Field.CondensationAlpha = 0.05f;
	smallerFainter.GpuParticles->Field.CondensationSize = 0.5f;
	fixture.Render.Render(std::span(&smallerFainter, 1), overlay, nullptr, false);
	const CapturedImage smallerFainterImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		smallerFainter.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	CHECK(ChangedBytes(redImage, smallerFainterImage) > 64);
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
	const CapturedImage refusedImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		refused.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	// The refused request changes the authored field back to its default grey
	// condensation. Red pixels prove the retained GPU state and its preceding
	// authored presentation values still draw after allocation fails.
	CHECK(ChangedBytes(empty, refusedImage) > 64);
	CHECK(RedDominantPixels(refusedImage, empty) > 64);

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
