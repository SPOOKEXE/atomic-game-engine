#include <engine/assets/Texture.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

TEST_SUITE_ID("client.imagegraphribbonbindings")
TEST_DEPENDS("client.imagegraphruntime")

namespace {
	using namespace engine;
	constexpr uint32_t SIDE = 32;
	const core::Name OWNER("imagegraph-ribbon-entity-world");
	const core::Name OTHER_OWNER("imagegraph-ribbon-other-world");
	const core::Name GRAPH("ribbon-entities");
	const core::Name BEAM_TEXTURE("imagegraph-beam-entity");
	const core::Name TRAIL_TEXTURE("imagegraph-trail-entity");
	const core::Name PIPELINE("imagegraph-ribbon-entity-pipeline");

	struct TestAssets {
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() / "atomic-imagegraph-ribbon-entities";
		TestAssets() {
			std::filesystem::remove_all(Root);
			std::filesystem::create_directories(Root / "imagegraphs");
			std::ofstream file(client::ImageGraphDocumentPath(Root, GRAPH));
			file << "imagegraph 1\n"
					"node \"beam\" \"image.solid\" \"\" 0 0\n"
					"value 0 \"beam\" \"width\" i 2\n"
					"value 0 \"beam\" \"height\" i 2\n"
					"value 0 \"beam\" \"colour\" c 255 0 0 255\n"
					"keyframe \"beam\" \"colour\" 0 \"step\" c 255 0 0 255\n"
					"keyframe \"beam\" \"colour\" 2 \"step\" c 0 255 0 255\n"
					"node \"trail\" \"image.solid\" \"\" 0 0\n"
					"value 1 \"trail\" \"width\" i 2\n"
					"value 1 \"trail\" \"height\" i 2\n"
					"value 1 \"trail\" \"colour\" c 0 0 255 255\n"
					"keyframe \"trail\" \"colour\" 0 \"step\" c 0 0 255 255\n"
					"keyframe \"trail\" \"colour\" 2 \"step\" c 255 255 0 255\n"
					"output \"beam-output\" \"beam\" \"image\"\n"
					"output \"trail-output\" \"trail\" \"image\"\n";
		}
		~TestAssets() {
			std::filesystem::remove_all(Root);
		}
	};

	struct Device {
		bool VideoReady = SDL_Init(SDL_INIT_VIDEO);
		render::Renderer Renderer;
		~Device() {
			Renderer.Shutdown();
			if (VideoReady) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	};

	void InstallCapturePipeline(render::Renderer &renderer) {
		auto document = graph::DefaultPbrDocument();
		const core::Name capture("imagegraph-ribbon-entity-capture-boundary");
		graph::NodeKindSpec kind;
		kind.Kind = capture;
		kind.Scope = graph::NodeScope::Frame;
		kind.Queue = graph::ExecutionQueue::Cpu;
		kind.Category = graph::NodeCategory::Output;
		kind.Inputs.push_back({.Name = core::Name("composed-image"), .Kind = graph::ResourceKind::Texture});
		kind.Inputs.push_back({.Name = core::Name("albedo"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(kind)));
		REQUIRE(renderer.InstallNodeHandler(capture, [](const graph::RunContext &) { return true; }));
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = capture,
			 .NodeKind = capture,
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads,
			 .Target = core::Name("composed-image"),
			 .Key = core::Name("composed-image")}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name("albedo"), .Key = core::Name("albedo")}
		);
		graph::RenderGraph built;
		core::Name offender;
		REQUIRE(graph::Build(document, built, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(PIPELINE, built));
	}

	std::vector<std::byte> Capture(render::Renderer &renderer) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture =
			static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(core::Name("composed-image"), 0));
		REQUIRE(device != nullptr);
		REQUIRE(texture != nullptr);
		const uint32_t rowBytes = (SIDE * 4 + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = rowBytes * SIDE;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *buffer) {
			SDL_ReleaseGPUTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			SDL_CreateGPUTransferBuffer(device, &info), releaseTransfer
		);
		REQUIRE(transfer != nullptr);
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command != nullptr);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = SIDE;
		source.h = SIDE;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = rowBytes / 4;
		destination.rows_per_layer = SIDE;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
		);
		REQUIRE(fence != nullptr);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped != nullptr);
		std::vector<std::byte> pixels(SIDE * SIDE * 4);
		for (uint32_t y = 0; y < SIDE; ++y)
			std::memcpy(pixels.data() + y * SIDE * 4, mapped + y * rowBytes, SIDE * 4);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return pixels;
	}

	size_t
	ColourPixels(const render::Renderer &renderer, std::span<const std::byte> pixels, int r, int g, int b) {
		const auto format = static_cast<SDL_GPUTextureFormat>(renderer.Backend().ColourFormat);
		const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
						  format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		size_t count = 0;
		for (size_t at = 0; at < pixels.size(); at += 4) {
			const int red = std::to_integer<int>(pixels[at + (bgra ? 2 : 0)]);
			const int green = std::to_integer<int>(pixels[at + 1]);
			const int blue = std::to_integer<int>(pixels[at + (bgra ? 0 : 2)]);
			count += std::abs(red - r) < 60 && std::abs(green - g) < 60 && std::abs(blue - b) < 60;
		}
		return count;
	}
}

TEST_CASE(
	"Beam and Trail entities sample separate changing live graph textures", "[client][imagegraph][gpu][.]"
) {
	TestAssets assets;
	scene::RegisterSceneClasses();
	effects::RegisterEffectClasses();
	ecs::Store store("imagegraph-ribbon-entities");
	const auto makeAttachment = [&](core::Vector3 position) {
		const auto entity = store.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "End");
		scene::Attachment attachment;
		attachment.Frame.Position = position;
		store.Set(entity, attachment);
		return entity;
	};
	const auto beamStart = makeAttachment({-1.5f, 1.0f, -4});
	const auto beamEnd = makeAttachment({1.5f, 1.0f, -4});
	const auto beamEntity = store.CreateInstance(ecs::Classes::Find(core::Name("Beam")), "LiveBeam");
	effects::Beam beam;
	beam.Attachment0 = beamStart;
	beam.Attachment1 = beamEnd;
	beam.Width0 = beam.Width1 = 0.8f;
	beam.Texture = BEAM_TEXTURE;
	store.Set(beamEntity, beam);
	const auto trailTop = makeAttachment({-1.5f, -0.6f, -4});
	const auto trailBottom = makeAttachment({-1.5f, -1.4f, -4});
	const auto trailEntity = store.CreateInstance(ecs::Classes::Find(core::Name("Trail")), "LiveTrail");
	effects::Trail trail;
	trail.Attachment0 = trailTop;
	trail.Attachment1 = trailBottom;
	trail.Texture = TRAIL_TEXTURE;
	store.Set(trailEntity, trail);
	REQUIRE(effects::RecordTrails(store, 1.0f / 60.0f) == 1);
	store.GetMutable<scene::Attachment>(trailTop)->Frame.Position.X = 1.5f;
	store.GetMutable<scene::Attachment>(trailBottom)->Frame.Position.X = 1.5f;
	REQUIRE(effects::RecordTrails(store, 1.0f / 60.0f) == 1);
	effects::RibbonBuffer ribbons;
	REQUIRE(effects::BuildRibbons(store, {0, 0, 0}, 0, ribbons) == 2);
	REQUIRE(ribbons.Runs.size() == 2);
	CHECK(ribbons.Runs[0].Texture == BEAM_TEXTURE);
	CHECK(ribbons.Runs[1].Texture == TRAIL_TEXTURE);
	CHECK(ribbons.Runs[0].Count >= 4);
	CHECK(ribbons.Runs[1].Count >= 4);

	const auto beamBinding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = core::Name("beam-output");
	selector.Texture = BEAM_TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	REQUIRE(scene::SetImageGraphBinding(store, beamBinding, selector));
	const auto trailBinding = store.Create();
	selector.Output = core::Name("trail-output");
	selector.Texture = TRAIL_TEXTURE;
	REQUIRE(scene::SetImageGraphBinding(store, trailBinding, selector));

	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	client::ImageGraphRuntime runtime;
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 88;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.CameraFrame = core::CFrame::LookAt({0, 0, 0}, {0, 0, -4});
	view.RibbonVertices = ribbons.Vertices;
	view.RibbonRuns = ribbons.Runs;
	render::OverlayImage overlay;
	const auto draw = [&] {
		view.Damage.Particles = true;
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(result.RibbonVertices == ribbons.Vertices.size());
		return Capture(device.Renderer);
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets.Root) == 2);
	assets::TextureData copied;
	CHECK(
		device.Renderer.CopyTexture(BEAM_TEXTURE, copied, 1024, OTHER_OWNER) ==
		render::TextureCopyStatus::Missing
	);
	(void)draw();
	const auto first = draw();
	CHECK(ColourPixels(device.Renderer, first, 255, 0, 0) > 0);
	CHECK(ColourPixels(device.Renderer, first, 0, 0, 255) > 0);
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets.Root) == 2);
	const auto second = draw();
	CHECK(ColourPixels(device.Renderer, second, 0, 255, 0) > 0);
	CHECK(ColourPixels(device.Renderer, second, 255, 255, 0) > 0);
	selector.Texture = TRAIL_TEXTURE;
	selector.Output = core::Name("missing-output");
	REQUIRE(scene::SetImageGraphBinding(store, trailBinding, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets.Root) == 0);
	CHECK(draw() == second);
	runtime.Clear(device.Renderer);
}
