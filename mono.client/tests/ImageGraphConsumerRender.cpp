#include "../../mono.engine/render/src/FrameBatch.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

TEST_SUITE_ID("client.imagegraphconsumer.render")
TEST_DEPENDS("client.imagegraphruntime")

namespace {
	using namespace engine;
	constexpr uint32_t SIDE = 32;
	const core::Name OWNER("imagegraph-gui-world");
	const core::Name GRAPH("gui-changing-solid");
	const core::Name OUTPUT("final");
	const core::Name TEXTURE("gui-graph-output");
	const core::Name PIPELINE("imagegraph-gui-pipeline");

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
		const core::Name capture("imagegraph-gui-capture-boundary");
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

	// Download the retained composed target after the renderer submits its GUI pass.
	std::vector<std::byte>
	CaptureComposed(render::Renderer &renderer, core::Name resource = core::Name("composed-image")) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(resource, 0));
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
		const auto releaseFence = [device](SDL_GPUFence *value) { SDL_ReleaseGPUFence(device, value); };
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

	std::array<std::byte, 4> DecodedPixel(
		const render::Renderer &renderer, const std::vector<std::byte> &pixels, uint32_t x, uint32_t y
	) {
		const size_t at = (static_cast<size_t>(y) * SIDE + x) * 4;
		std::array<std::byte, 4> rgba{pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
		const auto format = static_cast<SDL_GPUTextureFormat>(renderer.Backend().ColourFormat);
		if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
			format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
			std::swap(rgba[0], rgba[2]);
		}
		return rgba;
	}
}

TEST_CASE(
	"ImageLabel samples changing graph pixels and holds last good result", "[client][imagegraph][gpu][.]"
) {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-gui-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 2 \"step\" c 0 255 0 255\n"
				"output \"final\" \"solid\" \"image\"\n";
	}
	gui::RegisterGuiClasses();
	scene::RegisterSceneComponents();
	ecs::Store store("imagegraph-gui-world");
	const auto content = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "StarterGui");
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
	REQUIRE(store.SetParent(screen, content));
	const auto label = store.CreateInstance(gui::GuiClass("ImageLabel"), "GraphImage");
	REQUIRE(store.SetParent(label, screen));
	gui::Element element;
	element.Size = {1, 0, 1, 0};
	store.Set(label, element);
	REQUIRE(store.GetMutable<gui::Picture>(label) != nullptr);
	store.GetMutable<gui::Picture>(label)->Image = TEXTURE;
	const auto binding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	gui::CompileRequest request;
	request.Display.Width = SIDE;
	request.Display.Height = SIDE;
	gui::Compiled compiled;
	REQUIRE(compiled.Rebuild(store, request));
	REQUIRE(
		std::any_of(
			compiled.Commands().Commands.begin(),
			compiled.Commands().Commands.end(),
			[](const auto &command) {
				return command.Kind == gui::DrawKind::Image && command.Image == TEXTURE;
			}
		)
	);

	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	render::InterfacePass interface;
	const auto backend = device.Renderer.Backend();
	REQUIRE(interface.Initialise(backend.Device, backend.ColourFormat));
	interface.SetContentOwner(OWNER);
	interface.SetImageSource([&](const core::Name &name) {
		render::InterfaceImage image;
		image.Texture = device.Renderer.TextureHandle(name, OWNER);
		(void)device.Renderer.TextureSize(name, image.Width, image.Height, OWNER);
		return image;
	});
	client::ImageGraphRuntime runtime;
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 1;
	view.WorldName = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	render::OverlayImage overlay;
	const auto draw = [&] {
		view.Damage.GameInterface = true;
		interface.Submit(compiled.Commands(), {SIDE, SIDE}, {SIDE, SIDE}, store, compiled.Signature());
		device.Renderer.Render(std::span(&view, 1), overlay, &interface, false);
		return CaptureComposed(device.Renderer);
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	CHECK(runtime.DocumentParses() == 1);
	(void)draw();
	const auto red = draw();
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	CHECK(runtime.DocumentParses() == 1);
	const auto green = draw();
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	// The display target's red and blue byte order follows its backend format.
	// Green is the same byte in either ordering.
	CHECK(std::max(std::to_integer<int>(red[centre]), std::to_integer<int>(red[centre + 2])) > 180);
	CHECK(std::to_integer<uint8_t>(red[centre + 1]) < 70);
	CHECK(std::to_integer<uint8_t>(green[centre]) < 70);
	CHECK(std::to_integer<uint8_t>(green[centre + 2]) < 70);
	CHECK(std::to_integer<uint8_t>(green[centre + 1]) > 180);
	selector.Output = core::Name("missing");
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets) == 0);
	CHECK(draw() == green);
	runtime.Clear(device.Renderer);
	interface.Shutdown();
	std::filesystem::remove_all(assets);
}

TEST_CASE("ImageLabel publishes an authored Transform Image 3D GPU result", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-transform-3d-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 6\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 11 22 33 255\n"
				"node \"transform\" \"image.transform_3d\" \"\" 0 0\n"
				"value 1 \"transform\" \"position\" 3 0 0 -1\n"
				"value 1 \"transform\" \"anchor\" 3 0 0 0\n"
				"value 1 \"transform\" \"rotation\" h 0 0 0 1\n"
				"value 1 \"transform\" \"scale\" 3 1 1 1\n"
				"value 1 \"transform\" \"texture_tiling\" v 1 1\n"
				"value 1 \"transform\" \"projection\" e 1\n"
				"value 1 \"transform\" \"fov\" d 45\n"
				"value 1 \"transform\" \"view_range\" v 0.001 10\n"
				"value 1 \"transform\" \"depth_range\" v 0 1\n"
				"link \"solid\" \"image\" \"transform\" \"surface\"\n"
				"output \"final\" \"transform\" \"rendered\"\n";
	}
	gui::RegisterGuiClasses();
	scene::RegisterSceneComponents();
	ecs::Store store("imagegraph-transform-3d-world");
	const auto content = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "StarterGui");
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
	REQUIRE(store.SetParent(screen, content));
	const auto label = store.CreateInstance(gui::GuiClass("ImageLabel"), "GraphImage");
	REQUIRE(store.SetParent(label, screen));
	gui::Element element;
	element.Size = {1, 0, 1, 0};
	store.Set(label, element);
	REQUIRE(store.GetMutable<gui::Picture>(label) != nullptr);
	store.GetMutable<gui::Picture>(label)->Image = TEXTURE;
	const auto binding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::Fixed;
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	gui::CompileRequest request;
	request.Display.Width = SIDE;
	request.Display.Height = SIDE;
	gui::Compiled compiled;
	REQUIRE(compiled.Rebuild(store, request));
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	render::InterfacePass interface;
	const auto backend = device.Renderer.Backend();
	REQUIRE(interface.Initialise(backend.Device, backend.ColourFormat));
	interface.SetContentOwner(OWNER);
	interface.SetImageSource([&](const core::Name &name) {
		render::InterfaceImage image;
		image.Texture = device.Renderer.TextureHandle(name, OWNER);
		(void)device.Renderer.TextureSize(name, image.Width, image.Height, OWNER);
		return image;
	});
	const auto exported = client::LoadImageGraphRenderExportFrame(assets, GRAPH, OUTPUT, device.Renderer, 0);
	REQUIRE(exported.Status == imagegraph::Status::Ok);
	REQUIRE(exported.Image.Pixels.size() == 16);
	for (size_t index = 0; index < exported.Image.Pixels.size(); index += 4) {
		CHECK(exported.Image.Pixels[index] == 11);
		CHECK(exported.Image.Pixels[index + 1] == 22);
		CHECK(exported.Image.Pixels[index + 2] == 33);
		CHECK(exported.Image.Pixels[index + 3] == 255);
	}
	selector.Texture = core::Name("live-transform-rejected");
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets) == 0);
	CHECK(runtime.LastError() == "Transform Image 3D requires the headless export scheduler");
	runtime.Clear(device.Renderer);
	render::LiveImagePublisher publisher;
	const auto publication = publisher.BeginBinding(OWNER, TEXTURE);
	REQUIRE(publication.has_value());
	REQUIRE(
		publisher.Publish(
			device.Renderer,
			*publication,
			exported.Image.Width,
			exported.Image.Height,
			std::as_bytes(std::span(exported.Image.Pixels))
		) == render::LiveImagePublishStatus::Published
	);
	uint32_t width = 0, height = 0;
	REQUIRE(device.Renderer.TextureSize(TEXTURE, width, height, OWNER));
	CHECK(width == 2);
	CHECK(height == 2);
	CHECK(publisher.Retire(device.Renderer, *publication));
	interface.Shutdown();
	std::filesystem::remove_all(assets);
}

TEST_CASE("ImageButton samples live normal hover and pressed images", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-button-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 1 \"step\" c 0 255 0 255\n"
				"keyframe \"solid\" \"colour\" 2 \"step\" c 0 0 255 255\n"
				"output \"final\" \"solid\" \"image\"\n";
	}
	gui::RegisterGuiClasses();
	scene::RegisterSceneComponents();
	ecs::Store store("imagegraph-button-world");
	const auto content = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "StarterGui");
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
	REQUIRE(store.SetParent(screen, content));
	const auto button = store.CreateInstance(gui::GuiClass("ImageButton"), "GraphButton");
	REQUIRE(store.SetParent(button, screen));
	auto *element = store.GetMutable<gui::Element>(button);
	REQUIRE(element != nullptr);
	element->Size = {1, 0, 1, 0};
	element->Interactable = true;
	const core::Name normal("graph-button-normal");
	const core::Name hover("graph-button-hover");
	const core::Name pressed("graph-button-pressed");
	auto *picture = store.GetMutable<gui::Picture>(button);
	REQUIRE(picture != nullptr);
	picture->Image = normal;
	picture->HoverImage = hover;
	picture->PressedImage = pressed;
	for (const auto &[name, tick] : {std::pair{normal, 0ull}, {hover, 1ull}, {pressed, 2ull}}) {
		const auto binding = store.Create();
		scene::ImageGraphBinding selector;
		selector.Graph = GRAPH;
		selector.Output = OUTPUT;
		selector.Texture = name;
		selector.TickPolicy = scene::ImageGraphTickPolicy::Fixed;
		selector.FixedTick = tick;
		REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	}
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	render::InterfacePass interface;
	const auto backend = device.Renderer.Backend();
	REQUIRE(interface.Initialise(backend.Device, backend.ColourFormat));
	interface.SetContentOwner(OWNER);
	interface.SetImageSource([&](const core::Name &name) {
		render::InterfaceImage image;
		image.Texture = device.Renderer.TextureHandle(name, OWNER);
		(void)device.Renderer.TextureSize(name, image.Width, image.Height, OWNER);
		return image;
	});
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 3);
	CHECK(runtime.DocumentParses() == 1);
	gui::CompileRequest request;
	request.Display.Width = SIDE;
	request.Display.Height = SIDE;
	gui::Compiled compiled;
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 2;
	view.WorldName = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	render::OverlayImage overlay;
	const auto capture = [&](ecs::Entity hovered, ecs::Entity pushed, core::Name expected) {
		request.Hovered = hovered;
		request.Pressed = pushed;
		(void)compiled.Rebuild(store, request);
		REQUIRE(
			std::any_of(
				compiled.Commands().Commands.begin(),
				compiled.Commands().Commands.end(),
				[&](const auto &command) {
					return command.Kind == gui::DrawKind::Image && command.Image == expected;
				}
			)
		);
		view.Damage.GameInterface = true;
		interface.Submit(compiled.Commands(), {SIDE, SIDE}, {SIDE, SIDE}, store, compiled.Signature());
		device.Renderer.Render(std::span(&view, 1), overlay, &interface, false);
		return CaptureComposed(device.Renderer);
	};
	(void)capture({}, {}, normal);
	const auto red = capture({}, {}, normal);
	const auto green = capture(button, {}, hover);
	const auto blue = capture(button, button, pressed);
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	CHECK(std::max(std::to_integer<int>(red[centre]), std::to_integer<int>(red[centre + 2])) > 180);
	CHECK(std::to_integer<int>(red[centre + 1]) < 70);
	CHECK(std::to_integer<int>(green[centre]) < 70);
	CHECK(std::to_integer<int>(green[centre + 2]) < 70);
	CHECK(std::to_integer<int>(green[centre + 1]) > 180);
	CHECK(std::to_integer<int>(blue[centre + 1]) < 70);
	CHECK(std::max(std::to_integer<int>(blue[centre]), std::to_integer<int>(blue[centre + 2])) > 180);
	runtime.Clear(device.Renderer);
	interface.Shutdown();
	std::filesystem::remove_all(assets);
}

TEST_CASE("ribbon samples a changing live graph texture", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-ribbon-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 2 \"step\" c 0 255 0 255\n"
				"output \"final\" \"solid\" \"image\"\n";
	}
	scene::RegisterSceneComponents();
	ecs::Store store("imagegraph-ribbon-world");
	const auto binding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	client::ImageGraphRuntime runtime;
	const std::array<effects::RibbonVertex, 4> vertices{{
		{{-2.0f, 1.5f, -4.0f}, {0.0f, 0.0f}, 0xFFFFFFFFu},
		{{-2.0f, -1.5f, -4.0f}, {0.0f, 1.0f}, 0xFFFFFFFFu},
		{{2.0f, 1.5f, -4.0f}, {1.0f, 0.0f}, 0xFFFFFFFFu},
		{{2.0f, -1.5f, -4.0f}, {1.0f, 1.0f}, 0xFFFFFFFFu},
	}};
	const std::array<effects::RibbonRun, 1> runs{{{.First = 0, .Count = 4, .Texture = TEXTURE}}};
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 3;
	view.WorldName = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.CameraFrame = core::CFrame::LookAt({0, 0, 0}, {0, 0, -4});
	view.ContentOwner = OWNER;
	view.RibbonVertices = vertices;
	view.RibbonRuns = runs;
	render::OverlayImage overlay;
	uint32_t drawCalls = 0;
	uint64_t triangles = 0;
	const auto capture = [&] {
		view.Damage.Particles = true;
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(result.RibbonVertices == 4);
		drawCalls = result.DrawCalls;
		triangles = result.Triangles;
		return CaptureComposed(device.Renderer);
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	(void)capture();
	const auto red = capture();
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	const auto green = capture();
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	size_t changedPixels = 0;
	for (size_t offset = 0; offset < red.size(); offset += 4)
		changedPixels += red[offset] != green[offset] || red[offset + 1] != green[offset + 1] ||
						 red[offset + 2] != green[offset + 2];
	CHECK(changedPixels > 0);
	CHECK(drawCalls > 0);
	CHECK(triangles >= 2);
	CHECK(std::max(std::to_integer<int>(red[centre]), std::to_integer<int>(red[centre + 2])) > 180);
	CHECK(std::to_integer<int>(red[centre + 1]) < 70);
	CHECK(std::to_integer<int>(green[centre]) < 70);
	CHECK(std::to_integer<int>(green[centre + 2]) < 70);
	CHECK(std::to_integer<int>(green[centre + 1]) > 180);
	selector.Output = core::Name("missing");
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets) == 0);
	CHECK(capture() == green);
	runtime.Clear(device.Renderer);
	std::filesystem::remove_all(assets);
}

TEST_CASE("material colour map samples a changing live graph", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-material-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 2 \"step\" c 0 255 0 255\n"
				"output \"final\" \"solid\" \"image\"\n";
	}
	scene::RegisterSceneComponents();
	scene::RegisterSceneClasses();
	ecs::Store store("imagegraph-material-world");
	const core::Name shaderName("imagegraph-material-sampler");
	const auto shader = store.CreateInstance(scene::ShaderScriptClass(), shaderName.Text());
	const auto authoredMaterial = store.CreateInstance(scene::MaterialClass(), "GraphMaterial");
	REQUIRE(shader != ecs::NULL_ENTITY);
	REQUIRE(authoredMaterial != ecs::NULL_ENTITY);
	store.GetMutable<scene::MaterialRef>(authoredMaterial)->Shader = shaderName;
	REQUIRE(
		scene::SetShaderSource(
			store,
			shader,
			R"glsl(#version 450
layout(location=4) in vec2 inTexCoord;
layout(location=0) out vec4 outColour;
layout(set=2,binding=2) uniform sampler2D colourMap;
void main(){outColour=texture(colourMap,inTexCoord);}
)glsl"
		)
	);
	const auto binding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	const auto material = store.Create();
	scene::SurfaceAppearance appearance;
	appearance.ColourMap = TEXTURE;
	store.Set(material, appearance);
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	render::ShaderLibrary shaderLibrary;
	REQUIRE(shaderLibrary.Refresh(store, OWNER) == 1);
	const std::array shaderDemand{shaderName};
	REQUIRE(device.Renderer.PrepareShaders(shaderLibrary, shaderDemand, {}, {}, OWNER));
	REQUIRE(device.Renderer.HasShader(shaderName, OWNER));
	InstallCapturePipeline(device.Renderer);
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("imagegraph-material-plane");
	REQUIRE(device.Renderer.AddMesh(mesh, plane, OWNER));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = mesh;
	instance.Texture = appearance.ColourMap;
	instance.Shader = shaderName;
	instance.Frame.Position = {0, 0, -3};
	instance.HalfExtent = {2, 2, .01f};
	instance.Tint = {1, 1, 1};
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 5;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	const auto capture = [&] {
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(result.Ran(core::Name("gbuffer")));
		return CaptureComposed(device.Renderer);
	};
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	(void)capture();
	const auto red = capture();
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	const auto green = capture();
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	// The composed Vulkan target stores BGRA bytes.
	CHECK(red[centre + 2] > std::byte{180});
	CHECK(red[centre + 1] < std::byte{70});
	CHECK(green[centre + 2] < std::byte{70});
	CHECK(green[centre + 1] > std::byte{180});
	REQUIRE(
		scene::SetShaderSource(
			store,
			shader,
			R"glsl(#version 450
layout(location=4) in vec2 inTexCoord;
layout(location=0) out vec4 outColour;
layout(set=2,binding=2) uniform sampler2D otherMap;
void main(){outColour=texture(otherMap,inTexCoord);}
)glsl"
		)
	);
	REQUIRE(shaderLibrary.Refresh(store, OWNER) == 1);
	CHECK_FALSE(device.Renderer.PrepareShaders(shaderLibrary, shaderDemand, {}, {}, OWNER));
	CHECK(device.Renderer.HasShader(shaderName, OWNER));
	const auto admittedLastGood = capture();
	CHECK(admittedLastGood[centre] == green[centre]);
	CHECK(admittedLastGood[centre + 1] == green[centre + 1]);
	selector.Output = core::Name("missing");
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets) == 0);
	const auto lastGood = capture();
	CHECK(lastGood[centre] == green[centre]);
	CHECK(lastGood[centre + 1] == green[centre + 1]);
	runtime.Clear(device.Renderer);
	std::filesystem::remove_all(assets);
}

TEST_CASE("skybox samples a changing live graph", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-skybox-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n";
		for (size_t index = 0; index < 6; ++index)
			file << "node \"face" << index << "\" \"image.solid\" \"\" 0 0\n";
		for (size_t index = 0; index < 6; ++index) {
			const std::string node = "face" + std::to_string(index);
			file << "value " << index << " \"" << node << "\" \"width\" i 2\n"
				 << "value " << index << " \"" << node << "\" \"height\" i 2\n"
				 << "value " << index << " \"" << node << "\" \"colour\" c 255 0 0 255\n"
				 << "keyframe \"" << node << "\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				 << "keyframe \"" << node << "\" \"colour\" 2 \"step\" c 0 255 0 255\n"
				 << "output \"output" << index << "\" \"" << node << "\" \"image\"\n";
		}
	}
	scene::RegisterSceneClasses();
	ecs::Store store("imagegraph-skybox-world");
	scene::InstallServices(store);
	const auto lighting = store.FindFirstRoot("Lighting");
	REQUIRE(lighting != ecs::NULL_ENTITY);
	const auto skybox = store.CreateInstance(ecs::Classes::Find(core::Name("SkyboxTextures")), "GraphSky");
	REQUIRE(store.SetParent(skybox, lighting));
	auto *faces = store.GetMutable<scene::SkyboxTextures>(skybox);
	REQUIRE(faces != nullptr);
	std::array<core::Name, 6> names{
		core::Name("skyface0"),
		core::Name("skyface1"),
		core::Name("skyface2"),
		core::Name("skyface3"),
		core::Name("skyface4"),
		core::Name("skyface5")
	};
	faces->Front = names[0];
	faces->Back = names[1];
	faces->Left = names[2];
	faces->Right = names[3];
	faces->Up = names[4];
	faces->Down = names[5];
	std::array<ecs::Entity, 6> bindings;
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	for (size_t index = 0; index < names.size(); ++index) {
		bindings[index] = store.Create();
		selector.Texture = names[index];
		selector.Output = core::Name("output" + std::to_string(index));
		REQUIRE(scene::SetImageGraphBinding(store, bindings[index], selector));
	}
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr, 1, true));
	InstallCapturePipeline(device.Renderer);
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 6;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.OverrideLighting = true;
	view.Lighting.EnvironmentState = scene::EnvironmentOf(store);
	REQUIRE(view.Lighting.EnvironmentState.Skybox == scene::SkyboxSource::Textures);
	render::OverlayImage overlay;
	const auto capture = [&] {
		(void)device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		return CaptureComposed(device.Renderer);
	};
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	const size_t firstPublish = runtime.Refresh(store, device.Renderer, OWNER, assets);
	INFO(runtime.LastError());
	REQUIRE(firstPublish == 6);
	assets::TextureData firstTexture;
	REQUIRE(
		device.Renderer.CopyTexture(names[0], firstTexture, 1024, OWNER) == render::TextureCopyStatus::Copied
	);
	CHECK(firstTexture.Pixels[0] == std::byte{255});
	(void)capture();
	const auto red = capture();
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 6);
	const auto green = capture();
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	// The composed target uses the backend's BGRA byte order in this capture.
	CHECK(red[centre + 2] > green[centre + 2]);
	CHECK(green[centre + 1] > red[centre + 1]);
	selector.Output = core::Name("missing");
	selector.Texture = names[5];
	REQUIRE(scene::SetImageGraphBinding(store, bindings[5], selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, device.Renderer, OWNER, assets) == 0);
	const auto lastGood = capture();
	CHECK(lastGood[centre + 2] == green[centre + 2]);
	CHECK(lastGood[centre + 1] == green[centre + 1]);
	for (const auto name : names) {
		assets::TextureData retained;
		REQUIRE(
			device.Renderer.CopyTexture(name, retained, 1024, OWNER) == render::TextureCopyStatus::Copied
		);
		CHECK(retained.Pixels[1] == std::byte{255});
	}
	runtime.Clear(device.Renderer);
	std::filesystem::remove_all(assets);
}

TEST_CASE("particle samples a changing live graph texture", "[client][imagegraph][gpu][.]") {
	const std::filesystem::path assets =
		std::filesystem::temp_directory_path() / "atomic-imagegraph-particle-render-test";
	std::filesystem::remove_all(assets);
	std::filesystem::create_directories(assets / "imagegraphs");
	{
		std::ofstream file(client::ImageGraphDocumentPath(assets, GRAPH));
		file << "imagegraph 1\n"
				"node \"solid\" \"image.solid\" \"\" 0 0\n"
				"value 0 \"solid\" \"width\" i 2\n"
				"value 0 \"solid\" \"height\" i 2\n"
				"value 0 \"solid\" \"colour\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 0 \"step\" c 255 0 0 255\n"
				"keyframe \"solid\" \"colour\" 2 \"step\" c 0 255 0 255\n"
				"output \"final\" \"solid\" \"image\"\n";
	}
	scene::RegisterSceneComponents();
	ecs::Store store("imagegraph-particle-world");
	const auto binding = store.Create();
	scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = scene::ImageGraphTickPolicy::World;
	REQUIRE(scene::SetImageGraphBinding(store, binding, selector));
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	client::ImageGraphRuntime runtime;
	effects::EmitterBlock block;
	block.Frame.Position = {0, 0, -4};
	block.First = 0;
	block.Capacity = 8;
	block.ParticleLimit = 8;
	for (size_t index = 0; index < effects::CURVE_SAMPLES; ++index) {
		block.Curves.Size[index] = 2.0f;
		block.Curves.Alpha[index] = 1.0f;
		block.Curves.Colour[index] = 0xFFFFFFFFu;
	}
	effects::EmitterSpawnState spawn;
	spawn.Lifetime = core::NumberRange{10.0f};
	effects::EmitterRuntime emitterRuntime;
	emitterRuntime.Requested = 1;
	render::ParticleBatch particle;
	particle.Block = &block;
	particle.Spawn = &spawn;
	particle.Runtime = &emitterRuntime;
	particle.Texture = TEXTURE;
	const std::array particles{particle};
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 4;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Particles = particles;
	view.ParticleBlocks = 1;
	view.ParticlePool = block.Capacity;
	view.ParticleRevision = 1;
	view.ParticleLayoutRevision = 1;
	view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60.0f;
	render::OverlayImage overlay;
	const auto capture = [&] {
		view.Damage.Particles = true;
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(result.Particles == block.Capacity);
		return CaptureComposed(device.Renderer);
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	(void)capture();
	const auto red = capture();
	view.ParticleDelta = 0.0f;
	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, device.Renderer, OWNER, assets) == 1);
	const auto green = capture();
	const size_t centre = (SIDE / 2 * SIDE + SIDE / 2) * 4;
	CHECK(red[centre + 1] < std::byte{70});
	CHECK(green[centre] < std::byte{70});
	CHECK(green[centre + 2] < std::byte{70});
	CHECK(green[centre + 1] > std::byte{180});
	runtime.Clear(device.Renderer);
	std::filesystem::remove_all(assets);
}

TEST_CASE("particle GPU draw samples frame 65 of a 16 by 16 atlas", "[client][imagegraph][gpu][.]") {
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	const core::Name textureName("m7-particle-atlas");
	assets::TextureData atlas;
	atlas.Width = 16;
	atlas.Height = 16;
	atlas.FlipbookSide = 16;
	atlas.FlipbookFrames = 65;
	atlas.FlipbookFrameRate = 64.0f;
	atlas.Pixels.assign(16 * 16 * 4, std::byte{0});
	const auto cell = [&](size_t index, std::array<std::byte, 4> rgba) {
		std::copy(rgba.begin(), rgba.end(), atlas.Pixels.begin() + index * 4);
	};
	for (size_t index = 0; index < 65; index++)
		cell(index, {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}});
	cell(0, {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}});
	cell(64, {std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}});
	REQUIRE(device.Renderer.AddTexture(textureName, atlas, OWNER));
	effects::EmitterBlock block;
	block.Frame.Position = {0, 0, -4};
	block.First = 0;
	block.Capacity = 8;
	block.ParticleLimit = 8;
	block.Flipbook = effects::FlipbookLayout::Grid16x16;
	block.FlipbookPlayback = effects::FlipbookMode::Loop;
	block.Frames = 65;
	block.FlipbookRate = 64.0f;
	for (size_t index = 0; index < effects::CURVE_SAMPLES; index++) {
		block.Curves.Size[index] = 2.0f;
		block.Curves.Alpha[index] = 1.0f;
		block.Curves.Colour[index] = 0xFFFFFFFFu;
	}
	effects::EmitterSpawnState spawn;
	spawn.Lifetime = core::NumberRange{2.0f};
	effects::EmitterRuntime emitterRuntime;
	emitterRuntime.Requested = 1;
	render::ParticleBatch particle;
	particle.Block = &block;
	particle.Spawn = &spawn;
	particle.Runtime = &emitterRuntime;
	particle.Texture = textureName;
	particle.FlipbookSide = 16.0f;
	const std::array particles{particle};
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 65;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Particles = particles;
	view.ParticleBlocks = 1;
	view.ParticlePool = block.Capacity;
	view.ParticleRevision = 1;
	view.ParticleLayoutRevision = 1;
	view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60.0f;
	render::OverlayImage overlay;
	const auto capture = [&] {
		view.Damage.Particles = true;
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(result.Particles == block.Capacity);
		return CaptureComposed(device.Renderer);
	};
	(void)capture();
	const auto first = capture();
	// Two prior captures advanced the newborn by 2/60 seconds. Land inside
	// cell 64 with margin from the floating-point frame boundary.
	view.ParticleDelta = 1.0f - 2.0f / 60.0f + 0.001f;
	const auto sixtyFifth = capture();
	const auto early = DecodedPixel(device.Renderer, first, SIDE / 2, SIDE / 2);
	const auto late = DecodedPixel(device.Renderer, sixtyFifth, SIDE / 2, SIDE / 2);
	// At 2/60 seconds a 64fps sheet is on an early blue frame. The later
	// sample reaches the authored green frame 64 before the 65-frame wrap.
	CHECK(early[0] < std::byte{70});
	CHECK(early[2] > std::byte{180});
	CHECK(late[0] < std::byte{70});
	CHECK(late[1] > std::byte{180});
}

TEST_CASE("particle GPU draw follows unequal atlas frame durations", "[client][imagegraph][gpu][.]") {
	Device device;
	REQUIRE(device.VideoReady);
	REQUIRE(device.Renderer.Initialise(nullptr));
	InstallCapturePipeline(device.Renderer);
	const core::Name textureName("m7-variable-particle-atlas");
	const std::filesystem::path baked = core::Paths::Base() / "m7-variable/three-colour.atex";
	REQUIRE(std::filesystem::is_regular_file(baked));
	const size_t byteCount = std::filesystem::file_size(baked);
	REQUIRE(byteCount <= 1024 * 1024);
	std::ifstream file(baked, std::ios::binary);
	REQUIRE(file);
	std::vector<std::byte> bytes(byteCount);
	file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	REQUIRE(file.good());
	core::ByteReader reader(bytes);
	assets::TextureData atlas;
	REQUIRE(assets::Texture::Read(reader, atlas));
	REQUIRE(atlas.FlipbookSide == 2);
	REQUIRE(atlas.FlipbookFrames == 3);
	REQUIRE(atlas.FlipbookFrameDurations == std::vector<float>{0.04f, 0.10f, 0.06f});
	REQUIRE(device.Renderer.AddTexture(textureName, atlas, OWNER));
	effects::EmitterBlock block;
	block.Frame.Position = {0, 0, -4};
	block.First = 0;
	block.Capacity = 8;
	block.ParticleLimit = 8;
	block.Flipbook = effects::FlipbookLayout::Grid2x2;
	block.FlipbookPlayback = effects::FlipbookMode::Loop;
	block.Frames = 3;
	block.VariableFlipbookTiming = true;
	block.FlipbookTexture = textureName;
	for (size_t index = 0; index < effects::CURVE_SAMPLES; index++) {
		block.Curves.Size[index] = 2.0f;
		block.Curves.Alpha[index] = 1.0f;
		block.Curves.Colour[index] = 0xFFFFFFFFu;
	}
	effects::EmitterSpawnState spawn;
	spawn.Lifetime = core::NumberRange{1.0f};
	effects::EmitterRuntime emitterRuntime;
	emitterRuntime.Requested = 1;
	render::ParticleBatch particle;
	particle.Block = &block;
	particle.Spawn = &spawn;
	particle.Runtime = &emitterRuntime;
	particle.Texture = textureName;
	particle.FlipbookSide = 2.0f;
	const std::array particles{particle};
	render::SceneTarget target{SIDE, SIDE};
	render::View view;
	view.World = 66;
	view.WorldName = OWNER;
	view.ContentOwner = OWNER;
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Particles = particles;
	view.ParticleBlocks = 1;
	view.ParticlePool = block.Capacity;
	view.ParticleRevision = 1;
	view.ParticleLayoutRevision = 1;
	view.ParticleResidentRevision = 1;
	view.ParticleDelta = 1.0f / 60.0f;
	render::OverlayImage overlay;
	std::vector<uint32_t> capturedCells;
	const auto capture = [&] {
		view.Damage.Particles = true;
		const auto result = device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(result.Particles == block.Capacity);
		const auto cellId = render::FrameBatch::ParticleCellForTests(device.Renderer, 66, OWNER, 0);
		REQUIRE(cellId.has_value());
		capturedCells.push_back(*cellId);
		return CaptureComposed(device.Renderer);
	};
	(void)capture();
	const auto first = capture();
	view.ParticleDelta = 0.02f;
	const auto second = capture();
	view.ParticleDelta = 0.10f;
	const auto third = capture();
	view.ParticleDelta = 0.06f;
	const auto wrapped = capture();
	const auto red = DecodedPixel(device.Renderer, first, SIDE / 2, SIDE / 2);
	const auto green = DecodedPixel(device.Renderer, second, SIDE / 2, SIDE / 2);
	const auto blue = DecodedPixel(device.Renderer, third, SIDE / 2, SIDE / 2);
	const auto redAgain = DecodedPixel(device.Renderer, wrapped, SIDE / 2, SIDE / 2);
	CHECK(capturedCells == std::vector<uint32_t>{0, 0, 1, 2, 0});
	CHECK(red[0] > std::byte{180});
	CHECK(green[1] > std::byte{180});
	CHECK(blue[2] > std::byte{180});
	CHECK(redAgain[0] > std::byte{180});

	// A second emitter may share the texture but must not borrow a timeline
	// whose authored frame count disagrees with its own block.
	effects::EmitterBlock otherBlock = block;
	otherBlock.First = block.Capacity;
	otherBlock.Frames = 2;
	effects::EmitterSpawnState otherSpawn = spawn;
	effects::EmitterRuntime otherRuntime;
	otherRuntime.Requested = 1;
	render::ParticleBatch otherParticle = particle;
	otherParticle.Index = 1;
	otherParticle.Block = &otherBlock;
	otherParticle.Spawn = &otherSpawn;
	otherParticle.Runtime = &otherRuntime;
	const std::array bothParticles{particle, otherParticle};
	view.Particles = bothParticles;
	view.ParticleBlocks = 2;
	view.ParticlePool = block.Capacity + otherBlock.Capacity;
	view.ParticleLayoutRevision++;
	view.ParticleResidentRevision++;
	view.ParticleRevision++;
	view.Damage.Particles = true;
	CHECK(device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false).Particles == block.Capacity);

	otherBlock.Frames = 3;
	otherBlock.Revision++;
	view.ParticleResidentRevision++;
	view.ParticleRevision++;
	view.Damage.Particles = true;
	CHECK(
		device.Renderer.Render(std::span(&view, 1), overlay, nullptr, false).Particles ==
		block.Capacity + otherBlock.Capacity
	);
}
