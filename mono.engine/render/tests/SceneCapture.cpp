// The capture boundary on both sides: cheap refusal cases for every machine,
// plus an opt-in headless device case for the ownership transfer and readback.
//
// **The success path needs a real device and is not faked.** The `[gpu]` case
// creates Vulkan shaders, pipelines, buffers and textures, dispatches the
// particle compute path, draws to an offscreen target, captures it and reads it
// back. The ordinary runner excludes that tag; `--gpu-tests` opts into the
// driver requirement.

#include "GpuHeap.hpp"

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.scenecapture")

namespace {
	struct VideoSubsystem {
		bool Ready = SDL_Init(SDL_INIT_VIDEO);

		~VideoSubsystem() {
			if (Ready) {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		}
	};
}

TEST_CASE("a capture with no device is refused rather than attempted", "[render]") {
	// No `Initialise`, so there is no device - the same arrangement
	// `tests/Passes.cpp` uses to exercise a contract on a build machine with no
	// GPU.
	engine::render::Renderer renderer;

	// **Refused, not crashed.** This is reachable in a real editor: the studio
	// asks for a capture from inside its draw, and a headless run has no device
	// at all.
	CHECK_FALSE(renderer.CaptureSceneTexture(0, engine::core::Name("studio.thumbnail/fox.amesh")));
}

TEST_CASE("a capture under an invalid name is refused", "[render]") {
	engine::render::Renderer renderer;

	// A default `Name` is the "nothing" value, and publishing a texture under it
	// would put an entry in the table that no lookup could ever name again -
	// device memory with no way to reach it and no way to drop it.
	CHECK_FALSE(renderer.CaptureSceneTexture(0, engine::core::Name()));
}

TEST_CASE("a capture from a slot that was never drawn into is refused", "[render]") {
	engine::render::Renderer renderer;

	// **A slot index past the end is the ordinary case rather than an error.**
	// Slots are created as they are drawn into, so asking about one the frame
	// never used is what a caller does when a preview has not had its turn in
	// the rotation yet - and the honest answer is "nothing to copy" rather than
	// a blank texture, which would cache an empty picture for ever.
	CHECK_FALSE(renderer.CaptureSceneTexture(64, engine::core::Name("studio.thumbnail/late.amesh")));
}

TEST_CASE("file capture requests expose and clear their pending state without a device", "[render]") {
	engine::render::Renderer renderer;
	CHECK_FALSE(renderer.CapturePending());

	renderer.RequestSceneCapture("scene.bmp", 3);
	CHECK(renderer.CapturePending());
	renderer.RequestSceneCapture({});
	CHECK_FALSE(renderer.CapturePending());

	renderer.RequestWindowCapture("studio.bmp");
	CHECK(renderer.CapturePending());
	renderer.RequestWindowCapture({});
	CHECK_FALSE(renderer.CapturePending());
}

TEST_CASE("headless Vulkan runs resource, particle, capture, and readback paths", "[render][gpu][.]") {
	using namespace engine;

	VideoSubsystem video;
	REQUIRE(video.Ready);
	render::Renderer renderer;
	INFO(SDL_GetError());
	REQUIRE(renderer.Initialise(nullptr));
	REQUIRE(renderer.IsHeadless());
	REQUIRE(renderer.BackendName() == "vulkan");

	auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
	REQUIRE(device != nullptr);

	const render::GpuMemoryStatistics resourceBaseline = renderer.MemoryStatistics();
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	bufferInfo.size = 4096;
	SDL_GPUBuffer *buffer = render::gpu::CreateBuffer(device, &bufferInfo);
	REQUIRE(buffer != nullptr);

	SDL_GPUTextureCreateInfo textureInfo{};
	textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
	textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	textureInfo.width = 16;
	textureInfo.height = 8;
	textureInfo.layer_count_or_depth = 1;
	textureInfo.num_levels = 1;
	SDL_GPUTexture *texture = render::gpu::CreateTexture(device, &textureInfo);
	REQUIRE(texture != nullptr);

	const render::GpuMemoryStatistics allocated = renderer.MemoryStatistics();
	CHECK(allocated.Buffers == resourceBaseline.Buffers + 1);
	CHECK(allocated.BufferBytes == resourceBaseline.BufferBytes + bufferInfo.size);
	CHECK(allocated.Textures == resourceBaseline.Textures + 1);
	CHECK(allocated.TextureBytes == resourceBaseline.TextureBytes + 16u * 8u * 4u);

	render::gpu::ReleaseBuffer(device, buffer);
	render::gpu::ReleaseTexture(device, texture);
	const render::GpuMemoryStatistics released = renderer.MemoryStatistics();
	CHECK(released.Buffers == resourceBaseline.Buffers);
	CHECK(released.BufferBytes == resourceBaseline.BufferBytes);
	CHECK(released.Textures == resourceBaseline.Textures);
	CHECK(released.TextureBytes == resourceBaseline.TextureBytes);
	CHECK(released.ReleasedBytes >= allocated.ReleasedBytes + bufferInfo.size + 16u * 8u * 4u);

	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("headless-gpu-test");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	effects::EmitterBlock block;
	block.Frame.Position = core::Vector3{0.0f, 0.0f, -4.0f};
	block.First = 0;
	block.Capacity = 8;
	block.ParticleLimit = 8;
	for (size_t index = 0; index < effects::CURVE_SAMPLES; index++) {
		block.Curves.Size[index] = 0.5f;
		block.Curves.Alpha[index] = 1.0f;
		block.Curves.Colour[index] = 0x00FFFFFFu;
	}
	effects::EmitterSpawnState spawn;
	spawn.Lifetime = core::NumberRange{1.0f};
	effects::EmitterRuntime runtime;
	runtime.Requested = 1;

	render::ParticleBatch particle;
	particle.Block = &block;
	particle.Spawn = &spawn;
	particle.Runtime = &runtime;
	particle.Index = 0;
	const std::array<render::ParticleBatch, 1> particles{particle};
	const render::SceneTarget target{64, 32};
	render::View view;
	view.Target = &target;
	view.Slot = 0;
	view.World = 71;
	view.WorldName = core::Name("headless-gpu-world");
	view.Pipeline = pipelineName;
	view.Particles = particles;
	view.ParticleRevision = 1;
	view.ParticleLayoutRevision = 1;
	view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60.0f;
	view.ParticleBlocks = 1;
	view.ParticlePool = block.Capacity;

	render::OverlayImage overlay;
	const core::Name inspectedResource("albedo");
	renderer.Inspect(inspectedResource, 0);
	const std::array<render::View, 1> views{view};
	const render::FrameResult frame = renderer.Render(views, overlay, nullptr, false);
	CHECK(frame.ComputeDispatches > 0);
	CHECK(frame.Particles == block.Capacity);

	const render::GpuMemoryStatistics particleResident = renderer.MemoryStatistics();
	CHECK(particleResident.Buffers > released.Buffers);
	CHECK(particleResident.BufferBytes > released.BufferBytes);

	const core::Name captureName("headless-gpu-capture");
	const uint64_t texturesBeforeCapture = renderer.MemoryStatistics().Textures;
	REQUIRE(renderer.CaptureSceneTexture(0, captureName));
	CHECK(renderer.TextureHandle(captureName) != nullptr);
	CHECK(renderer.MemoryStatistics().Textures == texturesBeforeCapture + 1);

	renderer.Inspect({});
	REQUIRE(SDL_WaitForGPUIdle(device));
	view.ParticleDelta = 0.0f;
	const std::array<render::View, 1> pollingViews{view};
	renderer.Render(pollingViews, overlay, nullptr, false);
	CHECK(renderer.SceneTexture(0) != nullptr);
	const render::Renderer::ReadbackImage readback = renderer.Readback();
	REQUIRE(readback.IsValid());
	CHECK(readback.Source == inspectedResource);
	CHECK(readback.Slot == 0);
	CHECK(readback.Width > 0);
	CHECK(readback.Height > 0);

	REQUIRE(SDL_WaitForGPUIdle(device));
	const uint64_t texturesBeforeDrop = renderer.MemoryStatistics().Textures;
	CHECK(renderer.DropTexture(captureName));
	CHECK(renderer.TextureHandle(captureName) == nullptr);
	CHECK(renderer.MemoryStatistics().Textures + 1 == texturesBeforeDrop);

	renderer.ForgetWorld(view.World, view.WorldName);
	const render::GpuMemoryStatistics worldReleased = renderer.MemoryStatistics();
	CHECK(worldReleased.Buffers < particleResident.Buffers);
	CHECK(worldReleased.BufferBytes < particleResident.BufferBytes);

	renderer.Shutdown();
	CHECK(renderer.MemoryStatistics().LiveBytes == 0);
}
