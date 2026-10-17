// Real-device execution coverage for the generic cosmetic field.

#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
		view.GpuParticles.emplace();
		view.GpuParticles->Field.RequestedCount = count;
		view.GpuParticles->Field.Seed = seed;
		view.GpuParticles->Field.VelocityResponse = 0;
		view.GpuParticles->Field.Styles[0].Size = 3;
		for (uint32_t sample = 0; sample < 256; ++sample) {
			const float x = static_cast<float>(sample % 16) * 4 - 30,
						y = static_cast<float>(sample / 16) * 8 + 100;
			view.GpuParticles->Field.SpawnSamples.push_back({{x, y, 0}, 15, {}, 0});
		}

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

	// Authored samples cover a visible rectangle for a meaningful pixel capture.
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
	CHECK(firstFrame.ParticlesDrawn >= 36'000 * 12);
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

	// An independent world with identical authored samples and seed must render identical reset pixels.
	auto repeated = FieldView(target, 262144, 17);
	repeated.World = 172;
	repeated.WorldName = core::Name("generic-field-repeat");
	fixture.Render.Render(std::span(&repeated, 1), overlay, nullptr, false);
	const auto repeatedImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		repeated.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	CHECK(repeatedImage.Bytes == field.Bytes);

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
	red.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::First);
	red.GpuParticles->Field.Styles[0].Colour = {1.0f, 0.0f, 0.0f};
	red.GpuParticles->Field.Styles[0].Alpha = 0.8f;
	red.GpuParticles->Field.Styles[0].Size = 3.0f;
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
	const PixelBounds envelope = RedParticleBounds(redImage, empty);
	CHECK(envelope.Found);
	CHECK(envelope.Height() > 20);
	CHECK(envelope.Width() > 10);
	// Updating authored local samples must change the rendered envelope without any native service.
	auto moved = red;
	for (auto &sample : moved.GpuParticles->Field.SpawnSamples)
		sample.Position.X += 150;
	fixture.Render.Render(std::span(&moved, 1), overlay, nullptr, false);
	const CapturedImage movedImage = CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		moved.Slot,
		target.Width,
		target.Height,
		ImageFormat::Bgra8Unorm
	);
	CHECK(ChangedBytes(redImage, movedImage) > 64);

	auto smallerFainter = FieldView(target, 262'144, 17);
	smallerFainter.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::First);
	smallerFainter.GpuParticles->Field.Styles[0].Colour = {1.0f, 0.0f, 0.0f};
	smallerFainter.GpuParticles->Field.Styles[0].Alpha = 0.05f;
	smallerFainter.GpuParticles->Field.Styles[0].Size = 0.5f;
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
	// The allocation path must retain the working 262k field when the largest
	// optional preset cannot be admitted. This is the safe P50M fallback rather
	// than a partial replacement of the active field.
	render::test_support::SetForceGpuParticleFieldAllocationFailure(true);
	auto refused = FieldView(target, 50'000'000, 19);
	const render::FrameResult refusedFrame =
		fixture.Render.Render(std::span(&refused, 1), overlay, nullptr, false);
	CHECK(refusedFrame.ComputeDispatches == 0);
	CHECK(refusedFrame.ParticlesDrawn >= 36'000 * 12);
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
	// Allocation refusal retains the prior resident population and authored styles.
	CHECK(refusedImage.Bytes == smallerFainterImage.Bytes);

	auto resized = FieldView(target, 1'048'576, 23);
	const render::FrameResult resizedFrame =
		fixture.Render.Render(std::span(&resized, 1), overlay, nullptr, false);
	CHECK(resizedFrame.ComputeDispatches >= 1);
	// Resident simulation still owns one million rows while drawing a bounded cohort.
	CHECK(resizedFrame.ParticlesDrawn >= 36'000 * 12);
	CHECK(resizedFrame.ParticlesDrawn < 1'048'576);
	const render::GpuMemoryStatistics resizedMemory = fixture.Render.MemoryStatistics();
	// State is 32 bytes per row. The bounded sample upload does not grow with population.
	CHECK(resizedMemory.BufferBytes >= firstMemory.BufferBytes + (1'048'576u - 262'144u) * 32ull);
	CHECK(resizedMemory.TransferBufferBytes == firstMemory.TransferBufferBytes);
}

TEST_CASE(
	"GPU particle field advances once for two views of the same world", "[render][gpu][gpu-field-step][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");
	render::SceneTarget target{320, 180};
	render::OverlayImage overlay;
	auto reference = FieldView(target, 262144, 17);
	reference.World = 821;
	reference.WorldName = core::Name("field-single-camera");
	reference.ParticleDelta = .25f;
	reference.GpuParticles->Field.Styles[0].Colour = {1, 0, 0};
	reference.GpuParticles->Field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::First);
	for (auto &sample : reference.GpuParticles->Field.SpawnSamples)
		sample.Velocity = {80, 0, 0};
	std::array<render::View, 2> together{reference, reference};
	for (size_t index = 0; index < together.size(); ++index) {
		together[index].World = 822;
		together[index].WorldName = core::Name("field-two-cameras");
		together[index].Slot = index + 1;
	}
	const auto capture = [&](size_t slot) {
		return CaptureResource(
			fixture.Render,
			core::Name("tonemapped"),
			slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		);
	};
	// Both viewports publish the same world delta, just as two Studio panels do.
	// The second camera must see the same resident population as the first.
	for (uint32_t frame = 0; frame < 3; ++frame) {
		INFO("presentation frame " << frame);
		REQUIRE(fixture.Render.Render(std::span(&reference, 1), overlay, nullptr, false).Submitted);
		const auto expected = capture(reference.Slot);
		const auto combined = fixture.Render.Render(together, overlay, nullptr, false);
		REQUIRE(combined.Submitted);
		CHECK(combined.ComputeDispatches == 1);
		CHECK(combined.ParticlesDrawn > 0);
		for (const auto &view : together) {
			const auto actual = capture(view.Slot);
			CHECK(ChangedBytes(expected, actual) == 0);
		}
	}
}

TEST_CASE(
	"paused GPU particle fields reuse resident state without simulation or sample staging",
	"[render][gpu][gpu-field-step][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");
	render::SceneTarget target{320, 180};
	render::OverlayImage overlay;
	auto view = FieldView(target, 262144, 17);
	const auto capture = [&] {
		return CaptureResource(
			fixture.Render,
			core::Name("tonemapped"),
			view.Slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		);
	};
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto expected = capture();
	view.ParticleDelta = 0;
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	};
	const auto prepared = counter("render.gpu_particle_field.sample_prepare_bytes");
	const auto uploaded = counter("render.gpu_particle_field.sample_upload_bytes");
	for (uint32_t frame = 0; frame < 3; ++frame) {
		const auto paused = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(paused.Submitted);
		CHECK(paused.ComputeDispatches == 0);
		CHECK(paused.ParticlesDrawn > 0);
		CHECK(ChangedBytes(expected, capture()) == 0);
	}
	CHECK(counter("render.gpu_particle_field.sample_prepare_bytes") == prepared);
	CHECK(counter("render.gpu_particle_field.sample_upload_bytes") == uploaded);
	// Authored updates remain live at zero delta; only the unchanged case skips.
	view.GpuParticles->Field.Styles[0].Colour = {1, 0, 0};
	const auto changed = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(changed.Submitted);
	CHECK(changed.ComputeDispatches == 1);
	CHECK(ChangedBytes(expected, capture()) > 64);
	const auto recoloured = capture();
	// Moving a narrow field must recycle its old, out-of-bounds population
	// even while paused, rather than leaving it at the prior field position.
	view.GpuParticles->Frame.Position.X += 150;
	view.GpuParticles->Field.HalfExtent.X = 1;
	const auto moved = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(moved.Submitted);
	CHECK(moved.ComputeDispatches == 1);
	CHECK(ChangedBytes(recoloured, capture()) > 64);
}

TEST_CASE(
	"invalid GPU particle fields cannot draw a resident population", "[render][gpu][gpu-field-step][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");
	render::SceneTarget target{320, 180};
	render::OverlayImage overlay;
	auto view = FieldView(target, 262144, 17);
	const auto capture = [&](size_t slot) {
		return CaptureResource(
			fixture.Render,
			core::Name("tonemapped"),
			slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		);
	};
	auto empty = view;
	empty.GpuParticles.reset();
	REQUIRE(fixture.Render.Render(std::span(&empty, 1), overlay, nullptr, false).Submitted);
	const auto baseline = capture(empty.Slot);
	const auto resident = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(resident.Submitted);
	CHECK(resident.ParticlesDrawn > 0);
	CHECK(ChangedBytes(baseline, capture(view.Slot)) > 64);
	auto invalid = view;
	invalid.GpuParticles->Field.SpawnSamples.front().Lifetime = -1;
	const auto refused = fixture.Render.Render(std::span(&invalid, 1), overlay, nullptr, false);
	REQUIRE(refused.Submitted);
	CHECK(refused.ComputeDispatches == 0);
	CHECK(refused.ParticlesDrawn == 0);
	CHECK(ChangedBytes(baseline, capture(invalid.Slot)) == 0);
	// Validation also precedes the same-frame reuse guard: another camera
	// cannot draw the first camera's resident data for a malformed packet.
	std::array<render::View, 2> together{view, invalid};
	together[1].Slot = 1;
	const auto combined = fixture.Render.Render(together, overlay, nullptr, false);
	REQUIRE(combined.Submitted);
	CHECK(combined.ComputeDispatches == 1);
	CHECK(combined.ParticlesDrawn == resident.ParticlesDrawn);
	CHECK(ChangedBytes(baseline, capture(together[1].Slot)) == 0);
}
