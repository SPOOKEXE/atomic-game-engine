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
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Part.hpp>
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

TEST_CASE("headless Vulkan particle pools grow to the host ceiling and release", "[render][gpu][.]") {
	using namespace engine;

	VideoSubsystem video;
	REQUIRE(video.Ready);
	render::Renderer renderer;
	INFO(SDL_GetError());
	REQUIRE(renderer.Initialise(nullptr));
	REQUIRE(renderer.IsHeadless());

	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("headless-particle-pool-test");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	ecs::Store store("headless-particle-pool-world");
	effects::RegisterEffectClasses();
	effects::InstallParticles(store, 4, 8);
	store.ResourceMutable<effects::ParticleSystem>()->DeviceStepped = true;

	scene::PartDesc description;
	description.Simulated = false;
	const ecs::Entity part = scene::MakePart(store, description);
	REQUIRE(part != ecs::NULL_ENTITY);

	const auto addEmitter = [&store, part]() {
		const ecs::Entity emitter =
			store.CreateInstance(ecs::Classes::Find(core::Name("ParticleEmitter")));
		REQUIRE(emitter != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(emitter, part));
		auto *settings = store.GetMutable<effects::ParticleEmitter>(emitter);
		REQUIRE(settings != nullptr);
		// Three particles with a one-second lifetime need four slots. This makes
		// each claim land exactly on the pool's four-row growth boundary.
		settings->Rate = 3.0f;
		settings->Lifetime = core::NumberRange{1.0f, 1.0f};
		return emitter;
	};

	const render::SceneTarget target{64, 32};
	render::View view;
	view.Target = &target;
	view.Slot = 0;
	view.World = 93;
	view.WorldName = core::Name(store.Name());
	view.Pipeline = pipelineName;
	render::OverlayImage overlay;
	const std::array<render::View, 1> emptyViews{view};
	renderer.Render(emptyViews, overlay, nullptr, false);
	const render::GpuMemoryStatistics warm = renderer.MemoryStatistics();

	render::ParticleFrame particles;
	const auto renderParticles = [&]() {
		scene::ResolveAttachments(store);
		effects::RefreshEmitters(store);
		const effects::ParticleStatistics statistics = effects::StepParticles(store, 1.0f / 60.0f);
		render::CollectParticleBatches(store, particles);
		view.Particles = particles.Batches;
		view.ParticleSeams = particles.Seams;
		view.ParticleRevision = particles.Revision;
		view.ParticleLayoutRevision = particles.LayoutRevision;
		view.ParticleResidentRevision = particles.ResidentRevision;
		view.ParticleDelta = 1.0f / 60.0f;
		view.ParticleBlocks = particles.BlockCount;
		view.ParticlePool = particles.Pool;
		const std::array<render::View, 1> views{view};
		return std::pair{statistics, renderer.Render(views, overlay, nullptr, false)};
	};

	addEmitter();
	const auto [initialStatistics, initialFrame] = renderParticles();
	const auto *system = store.Resource<effects::ParticleSystem>();
	REQUIRE(system != nullptr);
	CHECK(system->Capacity == 4);
	CHECK(system->Used == 4);
	CHECK(initialStatistics.EmittersRefused == 0);
	CHECK(initialFrame.ComputeDispatches > 0);
	CHECK(initialFrame.Particles == 4);
	const render::GpuMemoryStatistics initial = renderer.MemoryStatistics();
	CHECK(initial.BufferAllocations > warm.BufferAllocations);
	CHECK(initial.TransferBufferAllocations > warm.TransferBufferAllocations);

	addEmitter();
	const auto [grownStatistics, grownFrame] = renderParticles();
	system = store.Resource<effects::ParticleSystem>();
	REQUIRE(system != nullptr);
	CHECK(system->Capacity == 8);
	CHECK(system->MaximumCapacity == 8);
	CHECK(system->Used == 8);
	CHECK(grownStatistics.EmittersRefused == 0);
	CHECK(grownFrame.Particles == 8);
	const render::GpuMemoryStatistics grown = renderer.MemoryStatistics();
	CHECK(grown.BufferAllocations == initial.BufferAllocations + 1);
	CHECK(grown.TransferBufferAllocations == initial.TransferBufferAllocations + 1);
	CHECK(grown.BufferBytes == initial.BufferBytes + 4 * sizeof(effects::ParticleState));
	CHECK(grown.TransferBufferBytes == initial.TransferBufferBytes + 4 * sizeof(effects::ParticleState));

	addEmitter();
	const auto [fullStatistics, fullFrame] = renderParticles();
	system = store.Resource<effects::ParticleSystem>();
	REQUIRE(system != nullptr);
	CHECK(system->Capacity == 8);
	CHECK(system->Used == 8);
	CHECK(system->Blocks.size() == 2);
	CHECK(fullStatistics.EmittersRefused == 1);
	CHECK(fullStatistics.EmitterClaimAttempts == 1);
	CHECK(fullFrame.Particles == 8);
	const render::GpuMemoryStatistics full = renderer.MemoryStatistics();
	CHECK(full.BufferAllocations == grown.BufferAllocations);
	CHECK(full.TransferBufferAllocations == grown.TransferBufferAllocations);
	CHECK(full.BufferBytes == grown.BufferBytes);
	CHECK(full.TransferBufferBytes == grown.TransferBufferBytes);

	renderer.ForgetWorld(view.World, view.WorldName);
	const render::GpuMemoryStatistics released = renderer.MemoryStatistics();
	CHECK(released.Buffers == warm.Buffers);
	CHECK(released.TransferBuffers == warm.TransferBuffers);
	CHECK(released.BufferBytes == warm.BufferBytes);
	CHECK(released.TransferBufferBytes == warm.TransferBufferBytes);

	renderer.Shutdown();
	CHECK(renderer.MemoryStatistics().LiveBytes == 0);
}
