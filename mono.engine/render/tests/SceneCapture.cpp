// The capture boundary on both sides: cheap refusal cases for every machine,
// plus an opt-in headless device case for the ownership transfer and readback.
//
// **The success path needs a real device and is not faked.** The `[gpu]` case
// creates Vulkan shaders, pipelines, buffers and textures, dispatches the
// particle compute path, draws to an offscreen target, captures it and reads it
// back. The ordinary runner excludes that tag; `--gpu-tests` opts into the
// driver requirement.

#include "GpuHeap.hpp"
#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.render.scenecapture")
TEST_DEPENDS("engine.render.fixtures")

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

TEST_CASE("owned captures refuse an unavailable device and unknown tickets", "[render]") {
	engine::render::Renderer renderer;
	CHECK(renderer.RequestSceneCapture(engine::render::SceneCaptureRequest{}) == 0);
	CHECK_FALSE(renderer.TakeSceneCapture(0));
	CHECK_FALSE(renderer.TakeSceneCapture(1));
	CHECK_FALSE(renderer.CancelSceneCapture(0));
	CHECK_FALSE(renderer.CancelSceneCapture(1));
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
	renderer.RequestSceneCapture(std::filesystem::path{});
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
	view.ParticleRevision = 1;
	view.ParticleLayoutRevision = 1;
	view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60.0f;

	render::OverlayImage overlay;
	// A world can publish its initial empty particle snapshot before the first
	// emitter claims a block. The next frame must stage that block even though
	// both snapshots begin their revision counters at one.
	const std::array<render::View, 1> emptyViews{view};
	const render::FrameResult emptyFrame = renderer.Render(emptyViews, overlay, nullptr, false);
	CHECK(emptyFrame.Particles == 0);

	view.Particles = particles;
	view.ParticleBlocks = 1;
	view.ParticlePool = block.Capacity;
	const core::Name inspectedResource("albedo");
	renderer.Inspect(inspectedResource, 0);
	const std::array<render::View, 1> views{view};
	const render::FrameResult frame = renderer.Render(views, overlay, nullptr, false);
	CHECK(frame.Submitted);
	CHECK(frame.ComputeDispatches > 0);
	CHECK(frame.Particles == block.Capacity);
	CHECK(renderer.ResourceTexture(core::Name("first-surface-validity"), view.Slot) == nullptr);

	const render::GpuMemoryStatistics particleResident = renderer.MemoryStatistics();
	CHECK(particleResident.Buffers > released.Buffers);
	CHECK(particleResident.BufferBytes > released.BufferBytes);

	const core::Name captureName("headless-gpu-capture");
	const uint64_t texturesBeforeCapture = renderer.MemoryStatistics().Textures;
	REQUIRE(renderer.CaptureSceneTexture(0, captureName));
	CHECK(renderer.TextureHandle(captureName) != nullptr);
	CHECK_FALSE(renderer.TextureSamplesSRGB(captureName));
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

	// File and owned consumers share one native scene download and preserve its bytes.
	const auto captureToken = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	REQUIRE(captureToken != 0);
	CHECK_FALSE(renderer.TakeSceneCapture(captureToken));
	const auto capturePath = std::filesystem::temp_directory_path() /
							 ("atomic-scene-capture-" + std::to_string(captureToken) + ".bmp");
	struct RemoveCapture {
		std::filesystem::path Path;
		~RemoveCapture() {
			std::error_code ignored;
			std::filesystem::remove(Path, ignored);
		}
	} removeCapture{capturePath};
	renderer.RequestSceneCapture(capturePath, 0);
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	};
	const uint64_t downloadsBefore = counter("render.scene_capture.downloads");
	const uint64_t downloadBytesBefore = counter("render.scene_capture.download_bytes");
	const auto capturedFrame = renderer.Render(pollingViews, overlay, nullptr, true);
	REQUIRE(capturedFrame.Submitted);
	CHECK(counter("render.scene_capture.downloads") - downloadsBefore == 1);
	CHECK(counter("render.scene_capture.download_bytes") - downloadBytesBefore == 64 * 32 * 4);
	auto owned = renderer.TakeSceneCapture(captureToken);
	REQUIRE(owned);
	REQUIRE(owned->Status == render::SceneCaptureStatus::Ready);
	CHECK(owned->Format == render::ResourceImageFormat::RGBA8_UNorm);
	CHECK(owned->Width == 64);
	CHECK(owned->Height == 32);
	CHECK(owned->RowStride == 64 * 4);
	CHECK(owned->Pixels.size() == 64 * 32 * 4);
	CHECK(owned->Token == captureToken);
	CHECK(owned->Frame > 0);
	CHECK(owned->World == view.World);
	CHECK(owned->WorldName == view.WorldName.Text());
	CHECK(owned->ViewSlot == view.Slot);
	CHECK(owned->Pipeline == pipelineName);
	CHECK(owned->PipelineRevision > 0);
	CHECK(owned->SnapshotId.empty());
	CHECK(owned->CameraFrame.FuzzyEq(view.CameraFrame, 0));
	CHECK_FALSE(renderer.TakeSceneCapture(captureToken));
	CHECK_FALSE(renderer.CancelSceneCapture(captureToken));
	SDL_Surface *bmp = SDL_LoadBMP(capturePath.string().c_str());
	REQUIRE(bmp != nullptr);
	SDL_Surface *rgba = SDL_ConvertSurface(bmp, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(bmp);
	REQUIRE(rgba != nullptr);
	REQUIRE(rgba->w == static_cast<int>(owned->Width));
	REQUIRE(rgba->h == static_cast<int>(owned->Height));
	bool samePixels = true;
	for (uint32_t row = 0; row < owned->Height; ++row) {
		const auto *pixels = static_cast<const uint8_t *>(rgba->pixels) + row * rgba->pitch;
		samePixels =
			samePixels &&
			std::equal(pixels, pixels + owned->RowStride, owned->Pixels.begin() + row * owned->RowStride);
	}
	SDL_DestroySurface(rgba);
	CHECK(samePixels);

	CHECK(renderer.RequestSceneCapture(render::SceneCaptureRequest{.MaxBytes = 0}) == 0);
	std::array<uint64_t, 4> tickets{};
	for (auto &ticket : tickets) {
		ticket = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
		REQUIRE(ticket != 0);
		CHECK(ticket > captureToken);
	}
	CHECK(renderer.RequestSceneCapture(render::SceneCaptureRequest{}) == 0);
	for (const auto ticket : tickets)
		CHECK(renderer.CancelSceneCapture(ticket));
	for (const auto ticket : tickets)
		CHECK_FALSE(renderer.TakeSceneCapture(ticket));
	const auto oversized =
		renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0, .MaxBytes = 1});
	const auto absent = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 73});
	renderer.Render(pollingViews, overlay, nullptr, false);
	const auto refused = renderer.TakeSceneCapture(oversized);
	REQUIRE(refused);
	CHECK(refused->Status == render::SceneCaptureStatus::OverBudget);
	CHECK(refused->Pixels.empty());
	const auto missingView = renderer.TakeSceneCapture(absent);
	REQUIRE(missingView);
	CHECK(missingView->Status == render::SceneCaptureStatus::Failed);
	const auto staleTicket = renderer.RequestSceneCapture(
		render::SceneCaptureRequest{.ViewSlot = 0, .ExpectedSnapshotId = "stale-snapshot"}
	);
	const auto exactEmptyTicket = renderer.RequestSceneCapture(
		render::SceneCaptureRequest{.ViewSlot = 0, .ExpectedSnapshotId = std::string{}}
	);
	const auto downloadsBeforeStale = counter("render.scene_capture.downloads");
	REQUIRE(renderer.Render(pollingViews, overlay, nullptr, false).Submitted);
	const auto staleImage = renderer.TakeSceneCapture(staleTicket);
	const auto exactEmptyImage = renderer.TakeSceneCapture(exactEmptyTicket);
	REQUIRE(staleImage);
	REQUIRE(exactEmptyImage);
	CHECK(staleImage->Status == render::SceneCaptureStatus::Failed);
	CHECK(staleImage->Pixels.empty());
	CHECK(exactEmptyImage->Status == render::SceneCaptureStatus::Ready);
	CHECK(exactEmptyImage->SnapshotId.empty());
	CHECK(counter("render.scene_capture.downloads") - downloadsBeforeStale == 1);
	CHECK(
		renderer.RequestSceneCapture(
			render::SceneCaptureRequest{.ExpectedSnapshotId = std::string(4097, 'x')}
		) == 0
	);

	// Provenance describes the raster lens after portal clipping is resolved.
	render::PortalView closePortal;
	closePortal.ExternalImage = true;
	closePortal.Centre = {0, 0, -.02f};
	closePortal.Normal = {0, 0, 1};
	closePortal.First = {1, 0, 0};
	closePortal.Second = {0, 1, 0};
	const std::array closePortals{closePortal};
	auto rasterView = view;
	rasterView.Camera.NearPlane = .5f;
	rasterView.Portals = closePortals;
	const auto clippedTicket = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	REQUIRE(renderer.Render(std::span(&rasterView, 1), overlay, nullptr, false).Submitted);
	const auto clippedImage = renderer.TakeSceneCapture(clippedTicket);
	REQUIRE(clippedImage);
	REQUIRE(clippedImage->Status == render::SceneCaptureStatus::Ready);
	const float clippedNear = scene::PortalNearPlane(.5f, .02f);
	CHECK(clippedImage->Camera.NearPlane == clippedNear);
	const auto clippedMatrices = scene::ResolveCamera(rasterView.CameraFrame, clippedImage->Camera, 2.0f);
	CHECK(clippedImage->Projection == clippedMatrices.Projection);
	// A fitted projection deliberately bypasses the portal near-plane adjustment.
	rasterView.Projection = scene::ResolveCamera(rasterView.CameraFrame, rasterView.Camera, 2.0f).Projection;
	const auto fittedTicket = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	REQUIRE(renderer.Render(std::span(&rasterView, 1), overlay, nullptr, false).Submitted);
	const auto fittedImage = renderer.TakeSceneCapture(fittedTicket);
	REQUIRE(fittedImage);
	REQUIRE(fittedImage->Status == render::SceneCaptureStatus::Ready);
	CHECK(fittedImage->Camera.NearPlane == rasterView.Camera.NearPlane);
	CHECK(fittedImage->Projection == *rasterView.Projection);

	// A nonfinal view's pixels must survive until the shared batch's final fence.
	auto secondView = view;
	secondView.Slot = 1;
	secondView.World = 72;
	secondView.WorldName = core::Name("headless-second-world");
	secondView.SnapshotId = "second-world-snapshot";
	const std::array pairedViews{view, secondView};
	const auto firstTicket = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	const auto secondTicket = renderer.RequestSceneCapture(
		render::SceneCaptureRequest{.ViewSlot = 1, .ExpectedSnapshotId = secondView.SnapshotId}
	);
	REQUIRE(renderer.Render(pairedViews, overlay, nullptr, false).Submitted);
	const auto firstImage = renderer.TakeSceneCapture(firstTicket);
	const auto secondImage = renderer.TakeSceneCapture(secondTicket);
	REQUIRE(firstImage);
	REQUIRE(secondImage);
	CHECK(firstImage->Status == render::SceneCaptureStatus::Ready);
	CHECK(secondImage->Status == render::SceneCaptureStatus::Ready);
	CHECK(firstImage->Frame == secondImage->Frame);
	CHECK(firstImage->World == view.World);
	CHECK(secondImage->World == secondView.World);
	CHECK(firstImage->ViewSlot == 0);
	CHECK(secondImage->ViewSlot == 1);
	CHECK(secondImage->SnapshotId == secondView.SnapshotId);

#if ENGINE_ASSERTS_ENABLED
	const auto failedSubmit = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	render::test_support::SetFrameBatchSubmissionFailureForTests(true);
	CHECK_FALSE(renderer.Render(pollingViews, overlay, nullptr, false).Submitted);
	const auto submitFailure = renderer.TakeSceneCapture(failedSubmit);
	REQUIRE(submitFailure);
	CHECK(submitFailure->Status == render::SceneCaptureStatus::Failed);
	CHECK(submitFailure->Pixels.empty());

#endif

	const auto skipped = renderer.RequestSceneCapture(render::SceneCaptureRequest{});
	renderer.Render({}, overlay, nullptr, false);
	const auto skippedResult = renderer.TakeSceneCapture(skipped);
	REQUIRE(skippedResult);
	CHECK(skippedResult->Status == render::SceneCaptureStatus::Failed);
	const auto retained = renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = 0});
	renderer.Render(pollingViews, overlay, nullptr, false);
	CHECK(renderer.CancelSceneCapture(retained));
	CHECK_FALSE(renderer.TakeSceneCapture(retained));

	renderer.ForgetWorld(secondView.World, secondView.WorldName);
	const core::Name skyHistory("environment-sky");
	REQUIRE(renderer.ResourceTexture(skyHistory, view.Slot) != nullptr);
	const uint64_t texturesBeforeWorldDrop = renderer.MemoryStatistics().Textures;
	renderer.ForgetWorld(view.World, core::Name("other-world"));
	CHECK(renderer.ResourceTexture(skyHistory, view.Slot) != nullptr);
	CHECK(renderer.MemoryStatistics().Textures == texturesBeforeWorldDrop);
	renderer.ForgetWorld(view.World, view.WorldName);
	const render::GpuMemoryStatistics worldReleased = renderer.MemoryStatistics();
	CHECK(worldReleased.Buffers < particleResident.Buffers);
	CHECK(worldReleased.BufferBytes < particleResident.BufferBytes);
	CHECK(worldReleased.Textures < texturesBeforeWorldDrop);
	CHECK(renderer.ResourceTexture(skyHistory, view.Slot) == nullptr);

	const auto teardownTicket = renderer.RequestSceneCapture(render::SceneCaptureRequest{});
	renderer.Shutdown();
	CHECK(renderer.MemoryStatistics().LiveBytes == 0);
	CHECK_FALSE(renderer.TakeSceneCapture(teardownTicket));
	CHECK(owned->Pixels.size() == 64 * 32 * 4);
}

TEST_CASE(
	"live image particle textures resolve their world namespace before ordinary content aliases",
	"[render][gpu][particle-image-owner][.]"
) {
	using namespace engine;
	const core::Name textureName(
		GENERATE("imagegraph-instance://scope#image", "editable-image://4294967297")
	);
	const bool foreignContentOwner = GENERATE(false, true);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("particle-image-owner-pipeline"), worldName("particle-image-world");
	const core::Name contentOwner = foreignContentOwner ? core::Name("foreign-asset-alias") : core::Name{};
	INFO("live texture=" << textureName.Text() << " ordinary owner=" << contentOwner.Text());
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));
	assets::TextureData image;
	image.Width = image.Height = 1;
	image.Format = assets::TextureFormat::RGBA8_LINEAR;
	image.Pixels = {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
	REQUIRE(renderer.AddTexture(textureName, image, contentOwner));
	image.Pixels = {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
	REQUIRE(renderer.AddTexture(textureName, image, worldName));
	REQUIRE(
		renderer.TextureHandle(textureName, worldName) != renderer.TextureHandle(textureName, contentOwner)
	);
	effects::EmitterBlock block;
	block.Frame.Position = {0, 0, -4};
	block.Capacity = 1;
	block.ParticleLimit = 1;
	for (size_t index = 0; index < effects::CURVE_SAMPLES; index++) {
		block.Curves.Size[index] = 2;
		block.Curves.Alpha[index] = 1;
		block.Curves.Colour[index] = 0x00FFFFFFu;
	}
	effects::EmitterSpawnState spawn;
	spawn.Speed = core::NumberRange{0};
	spawn.Lifetime = core::NumberRange{10};
	effects::EmitterRuntime runtime;
	runtime.Requested = 1;
	render::ParticleBatch particle;
	particle.Block = &block;
	particle.Spawn = &spawn;
	particle.Runtime = &runtime;
	particle.Texture = textureName;
	particle.LightEmission = 1;
	particle.LightInfluence = 0;
	const std::array particles{particle};
	const render::SceneTarget target{64, 64};
	render::View view;
	view.Target = &target;
	view.World = 797;
	view.WorldName = worldName;
	view.ContentOwner = contentOwner;
	view.Pipeline = pipelineName;
	view.Particles = particles;
	view.ParticleBlocks = 1;
	view.ParticlePool = 1;
	view.ParticleRevision = view.ParticleLayoutRevision = view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60;
	render::OverlayImage overlay;
	const auto colourTicket =
		renderer.RequestSceneCapture(render::SceneCaptureRequest{.ViewSlot = view.Slot});
	const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto colourCapture = renderer.TakeSceneCapture(colourTicket);
	REQUIRE(colourCapture);
	REQUIRE(colourCapture->Status == render::SceneCaptureStatus::Ready);
	CHECK(colourCapture->Format == render::ResourceImageFormat::RGBA8_UNorm);

	REQUIRE(frame.Submitted);
	REQUIRE(frame.ComputeDispatches > 0);
	REQUIRE(frame.ParticlesDrawn > 0);
	const auto captured = render::test::CaptureResource(
		renderer, core::Name("composed-image"), 0, 64, 64, render::test::ImageFormat::Bgra8Unorm
	);
	const size_t centre = 32 * captured.RowStrideBytes + 32 * 4;
	const auto red = std::to_integer<uint8_t>(captured.Bytes[centre + 2]);
	const auto green = std::to_integer<uint8_t>(captured.Bytes[centre + 1]);
	const auto blue = std::to_integer<uint8_t>(captured.Bytes[centre]);
	INFO("particle centre RGB=" << unsigned(red) << ',' << unsigned(green) << ',' << unsigned(blue));
	CHECK(blue > 128);
	CHECK(red < 16);
	CHECK(blue > green + 64);
	const size_t ownedCentre = 32 * colourCapture->RowStride + 32 * 4;
	CHECK(colourCapture->Pixels[ownedCentre] == red);
	CHECK(colourCapture->Pixels[ownedCentre + 1] == green);
	CHECK(colourCapture->Pixels[ownedCentre + 2] == blue);
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
		const ecs::Entity emitter = store.CreateInstance(ecs::Classes::Find(core::Name("ParticleEmitter")));
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

TEST_CASE(
	"client particle pools sharing authored images remain independent", "[render][gpu][particle-source][.]"
) {
	using namespace engine;
	const bool genericField = GENERATE(false, true);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("replica-particle-source");
	REQUIRE(fixture.Render.SetPipeline(pipelineName, pipeline));
	const core::Name authoredWorld("replica-particle-authority");
	const core::Name texture(
		GENERATE("editable-image://particle-source", "imagegraph-instance://particle-source")
	);
	assets::TextureData image;
	image.Width = image.Height = 1;
	image.Format = assets::TextureFormat::RGBA8_LINEAR;
	image.Pixels = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
	REQUIRE(fixture.Render.AddTexture(texture, image, authoredWorld));
	std::array<effects::EmitterBlock, 3> blocks;
	std::array<effects::EmitterSpawnState, 3> spawn;
	std::array<effects::EmitterRuntime, 3> runtime;
	std::array<render::ParticleBatch, 3> batches;
	std::array<render::View, 3> views;
	const render::SceneTarget target{64, 64};
	for (size_t index = 0; index < views.size(); ++index) {
		auto &block = blocks[index];
		block.Frame.Position = {0, 0, -4};
		block.Capacity = block.ParticleLimit = 1;
		for (size_t curve = 0; curve < effects::CURVE_SAMPLES; ++curve) {
			block.Curves.Size[curve] = 2;
			block.Curves.Alpha[curve] = 1;
			block.Curves.Colour[curve] = index == 2 ? 0x0000FF00u : 0x00FFFFFFu;
		}
		spawn[index].Speed = core::NumberRange{0};
		spawn[index].Lifetime = core::NumberRange{10};
		runtime[index].Requested = 1;
		auto &batch = batches[index];
		batch.Block = &block;
		batch.Spawn = &spawn[index];
		batch.Runtime = &runtime[index];
		batch.Texture = texture;
		batch.LightEmission = 1;
		batch.LightInfluence = 0;
		auto &view = views[index];
		view.Target = &target;
		view.Slot = index;
		view.World = 790;
		view.WorldName = authoredWorld;
		view.Pipeline = pipelineName;
		view.ParticleWorld = 791 + index;
		view.ParticleWorldName = core::Name(
			index == 0	 ? "replica-particle-ada"
			: index == 1 ? "replica-particle-grace"
						 : "replica-particle-sam"
		);
		batch.SourceWorld = view.ParticleWorldName;
		if (index != 2) {
			image.Pixels =
				index == 0
					? std::vector<std::byte>{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}}
					: std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
			REQUIRE(fixture.Render.AddTexture(texture, image, batch.SourceWorld));
		}
		view.Particles = std::span(&batch, 1);
		view.ParticleBlocks = view.ParticlePool = 1;
		view.ParticleRevision = view.ParticleLayoutRevision = view.ParticleResidentRevision = 1;
		view.ParticleDelta = 1.0f / 60;
		if (genericField) {
			view.Particles = {};
			view.GpuParticles.emplace();
			auto &field = view.GpuParticles->Field;
			field.RequestedCount = 262144;
			field.Layers = static_cast<uint8_t>(scene::GpuParticleLayer::First);
			field.VelocityResponse = 0;
			field.Styles[0].Colour = index == 0	  ? core::Color3{1, 0, 0}
									 : index == 1 ? core::Color3{0, 0, 1}
												  : core::Color3{0, 1, 0};
			field.Styles[0].Alpha = .8f;
			field.Styles[0].Size = .4f;
			field.SpawnSamples.push_back({{0, 0, -4}, 15, {}, 0});
			view.Lighting.Ambient = {1, 1, 1};
			view.OverrideLighting = true;
		}
	}
	render::OverlayImage overlay;
	const auto frame = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(frame.Submitted);
	CHECK(frame.ComputeDispatches >= 3);
	if (genericField) CHECK(fixture.Render.MemoryStatistics().BufferBytes >= 3ull * 262144 * 32);
	for (size_t index = 0; index < views.size(); ++index) {
		// Frame composition runs only for the final view; compare each view's scene output.
		const auto captured = render::test::CapturePresentationResource(
			fixture.Render, core::Name("tonemapped"), index, 64, 64
		);
		const size_t centre = 32 * captured.RowStrideBytes + 32 * 4;
		const int red = std::to_integer<uint8_t>(captured.Bytes[centre]);
		const int blue = std::to_integer<uint8_t>(captured.Bytes[centre + 2]);
		const int green = std::to_integer<uint8_t>(captured.Bytes[centre + 1]);
		INFO("client=" << index << " red=" << red << " blue=" << blue);
		CHECK(
			(index == 0	  ? red > blue + 64
			 : index == 1 ? blue > red + 64
						  : green > red + 64 && green > blue + 64)
		);
	}
}

TEST_CASE(
	"compact particle emission preserves bursts, retirement, regrouping and recycled blocks",
	"[render][gpu][particle-emission][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("compact-particle-emission");
	REQUIRE(fixture.Render.SetPipeline(pipelineName, pipeline));
	std::array<effects::EmitterBlock, 2> blocks;
	std::array<effects::EmitterSpawnState, 2> spawns;
	std::array<effects::EmitterRuntime, 2> runtimes;
	std::array<render::ParticleBatch, 2> batches;
	for (uint32_t index = 0; index < blocks.size(); ++index) {
		auto &block = blocks[index];
		block.Frame.Position = {index == 0 ? -1.0f : 1.0f, 0, -4};
		block.First = index == 0 ? 13 : 173;
		block.Capacity = block.ParticleLimit = 101;
		block.Generation = 1;
		for (size_t curve = 0; curve < effects::CURVE_SAMPLES; ++curve) {
			block.Curves.Size[curve] = 1;
			block.Curves.Alpha[curve] = 1;
			block.Curves.Colour[curve] = index == 0 ? 0x000000FFu : 0x00FF0000u;
		}
		spawns[index].Speed = core::NumberRange{0};
		spawns[index].Lifetime = core::NumberRange{index == 0 ? 2.0f : .2f};
		runtimes[index].ContinuousRate = index == 0 ? 20 : 0;
		runtimes[index].Requested = index == 0 ? 0 : 1;
		auto &batch = batches[index];
		batch.Block = &block;
		batch.Spawn = &spawns[index];
		batch.Runtime = &runtimes[index];
		batch.Index = index == 0 ? 3 : 9;
		batch.LightEmission = 1;
		batch.LightInfluence = 0;
	}
	const render::SceneTarget target{96, 64};
	render::View view;
	view.Target = &target;
	view.World = 815;
	view.WorldName = core::Name("compact-particle-emission-world");
	view.Pipeline = pipelineName;
	view.Particles = batches;
	view.ParticleBlocks = 10;
	view.ParticlePool = 274;
	view.ParticleRevision = view.ParticleLayoutRevision = view.ParticleResidentRevision = 1;
	view.ParticleDelta = .05f;
	render::OverlayImage overlay;
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	};
	const auto renderFrame = [&] {
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Submitted);
		CHECK(frame.Particles == (view.Particles.empty() ? 0 : 202));
		return render::test::CapturePresentationResource(fixture.Render, core::Name("tonemapped"), 0, 96, 64);
	};
	const auto colourPixels = [](const render::test::CapturedImage &image, size_t channel) {
		uint32_t found = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t at = y * image.RowStrideBytes + x * 4;
				const int sampled = std::to_integer<uint8_t>(image.Bytes[at + channel]);
				const int other = std::max(
					std::to_integer<uint8_t>(image.Bytes[at + (channel + 1) % 3]),
					std::to_integer<uint8_t>(image.Bytes[at + (channel + 2) % 3])
				);
				if (sampled > other + 64) ++found;
			}
		}
		return found;
	};
	const auto emittedBefore = counter("render.particles.emission_dispatch_lanes");
	const auto integratedBefore = counter("render.particles.integration_dispatch_lanes");
	const auto uploadedBefore = counter("render.particles.emit_work_upload_bytes");
	const auto initial = renderFrame();
	CHECK(colourPixels(initial, 0) > 0);
	CHECK(colourPixels(initial, 2) > 0);
	CHECK(counter("render.particles.emission_dispatch_lanes") - emittedBefore == 64);
	CHECK(counter("render.particles.integration_dispatch_lanes") - integratedBefore == 256);
	CHECK(counter("render.particles.emit_work_upload_bytes") - uploadedBefore == 16);

	// The burst expires while the unmodified continuous emitter keeps advancing.
	const auto warmUploads = counter("render.particles.emit_work_upload_bytes");
	view.ParticleDelta = .25f;
	++view.ParticleRevision;
	const auto retired = renderFrame();
	CHECK(colourPixels(retired, 0) > 0);
	CHECK(colourPixels(retired, 2) == 0);
	CHECK(counter("render.particles.emit_work_upload_bytes") == warmUploads);

	// A monotonic burst request changes parameters without changing work layout.
	runtimes[1].Requested = 2;
	++blocks[1].Revision;
	++view.ParticleRevision;
	++view.ParticleResidentRevision;
	view.ParticleDelta = .05f;
	const auto secondBurst = renderFrame();
	CHECK(colourPixels(secondBurst, 0) > 0);
	CHECK(colourPixels(secondBurst, 2) > 0);
	CHECK(counter("render.particles.emit_work_upload_bytes") == warmUploads);

	// Recycling a block and regrouping its material must keep the new emission
	// row tied to its absolute state range, including holes in the block table.
	++blocks[1].Generation;
	++blocks[1].Revision;
	++blocks[1].CurveRevision;
	std::fill(std::begin(blocks[1].Curves.Colour), std::end(blocks[1].Curves.Colour), 0x0000FF00u);
	runtimes[1].Requested = 1;
	batches[1].Additive = true;
	++view.ParticleRevision;
	++view.ParticleResidentRevision;
	++view.ParticleLayoutRevision;
	const auto recycled = renderFrame();
	CHECK(colourPixels(recycled, 0) > 0);
	CHECK(colourPixels(recycled, 1) > 0);
	CHECK(colourPixels(recycled, 2) == 0);
	CHECK(counter("render.particles.emit_work_upload_bytes") - warmUploads == 16);

	view.Particles = {};
	++view.ParticleRevision;
	++view.ParticleLayoutRevision;
	renderFrame();
	view.Particles = batches;
	++blocks[1].Generation;
	++blocks[1].Revision;
	++view.ParticleRevision;
	++view.ParticleResidentRevision;
	++view.ParticleLayoutRevision;
	const auto rebound = renderFrame();
	CHECK(colourPixels(rebound, 0) > 0);
	CHECK(colourPixels(rebound, 1) > 0);
	fixture.Render.Shutdown();
	CHECK(fixture.Render.MemoryStatistics().LiveBytes == 0);
}

TEST_CASE(
	"particle emission admits only blocks whose device tables were staged",
	"[render][gpu][particle-emission][.]"
) {
	using namespace engine;
	constexpr uint32_t EMITTERS = 2100;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("budgeted-particle-emission");
	REQUIRE(fixture.Render.SetPipeline(pipelineName, pipeline));
	std::vector<effects::EmitterBlock> blocks(EMITTERS);
	std::vector<effects::EmitterSpawnState> spawns(EMITTERS);
	std::vector<effects::EmitterRuntime> runtimes(EMITTERS);
	std::vector<render::ParticleBatch> batches(EMITTERS);
	for (uint32_t index = 0; index < EMITTERS; ++index) {
		auto &block = blocks[index];
		block.Frame.Position = {0, 0, -4};
		block.First = index;
		block.Capacity = block.ParticleLimit = 1;
		block.Generation = 1;
		for (size_t curve = 0; curve < effects::CURVE_SAMPLES; ++curve) {
			block.Curves.Size[curve] = index == EMITTERS - 1 ? 1 : 0;
			block.Curves.Alpha[curve] = 1;
			block.Curves.Colour[curve] = 0x00FF0000u;
		}
		spawns[index].Speed = core::NumberRange{0};
		spawns[index].Lifetime = core::NumberRange{10};
		runtimes[index].Requested = 1;
		auto &batch = batches[index];
		batch.Block = &block;
		batch.Spawn = &spawns[index];
		batch.Runtime = &runtimes[index];
		batch.Index = index;
		batch.LightEmission = 1;
		batch.LightInfluence = 0;
	}
	const render::SceneTarget target{64, 64};
	render::View view;
	view.Target = &target;
	view.World = 816;
	view.WorldName = core::Name("budgeted-particle-emission-world");
	view.Pipeline = pipelineName;
	view.Particles = batches;
	view.ParticleBlocks = view.ParticlePool = EMITTERS;
	view.ParticleRevision = view.ParticleLayoutRevision = view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60;
	render::OverlayImage overlay;
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	};
	const auto emittedBefore = counter("render.particles.emission_dispatch_lanes");
	const auto uploadedBefore = counter("render.particles.emit_work_upload_bytes");
	const auto first = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(first.Submitted);
	CHECK(first.Particles == 2048);
	CHECK(counter("render.particles.emission_dispatch_lanes") - emittedBefore == 2048);
	CHECK(counter("render.particles.emit_work_upload_bytes") - uploadedBefore == 2048 * 8);
	const auto incomplete =
		render::test::CapturePresentationResource(fixture.Render, core::Name("tonemapped"), 0, 64, 64);
	const size_t centre = 32 * incomplete.RowStrideBytes + 32 * 4;
	CHECK(
		std::to_integer<uint8_t>(incomplete.Bytes[centre + 2]) <=
		std::to_integer<uint8_t>(incomplete.Bytes[centre]) + 64
	);
	const auto emittedWarm = counter("render.particles.emission_dispatch_lanes");
	const auto uploadedWarm = counter("render.particles.emit_work_upload_bytes");
	++view.ParticleRevision;
	const auto second = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(second.Submitted);
	CHECK(second.Particles == EMITTERS);
	CHECK(counter("render.particles.emission_dispatch_lanes") - emittedWarm == 2112);
	CHECK(counter("render.particles.emit_work_upload_bytes") - uploadedWarm == EMITTERS * 8);
	const auto admitted =
		render::test::CapturePresentationResource(fixture.Render, core::Name("tonemapped"), 0, 64, 64);
	CHECK(
		std::to_integer<uint8_t>(admitted.Bytes[centre + 2]) >
		std::to_integer<uint8_t>(admitted.Bytes[centre]) + 64
	);
}
