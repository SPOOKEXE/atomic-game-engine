#include "../../mono.engine/render/src/GpuHeap.hpp"
#include "../../mono.engine/render/src/ImageGraphTransform3DResident.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

TEST_SUITE_ID("client.imagegraph_argument_bindings")
TEST_DEPENDS("engine.imagegraph.source_argument")
TEST_DEPENDS("engine.scene.imagegraphbinding")
TEST_DEPENDS("engine.render.liveimagepublisher")
TEST_DEPENDS("engine.render.sourcetransformimage3d")

namespace {
	using namespace engine::imagegraph;
	const engine::core::Name GRAPH{"argument-binding"}, OUTPUT{"image"}, TEXTURE{"argument-binding-texture"},
		OWNER{"argument-binding-owner"};
	struct StagedShaderPaths {
		std::filesystem::path Previous = engine::core::Paths::Assets();
		StagedShaderPaths() {
			engine::core::Paths::SetAssetsOverride({});
		}
		~StagedShaderPaths() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};
	// Device fixtures read uploaded bytes directly; ordinary clients do not retain CPU texture copies.
	engine::assets::TextureData DownloadBinding(engine::render::Renderer &renderer) {
		engine::assets::TextureData image;
		REQUIRE(renderer.TextureSize(TEXTURE, image.Width, image.Height, OWNER));
		REQUIRE(image.Width <= 4);
		REQUIRE(image.Height == 1);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.TextureHandle(TEXTURE, OWNER));
		REQUIRE(device);
		REQUIRE(texture);
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = 256;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *buffer) {
			engine::render::gpu::ReleaseTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			engine::render::gpu::CreateTransferBuffer(device, &info), releaseTransfer
		);
		REQUIRE(transfer);
		const auto cancelCommand = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
			SDL_AcquireGPUCommandBuffer(device), cancelCommand
		);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = image.Width;
		source.h = source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = 64;
		destination.rows_per_layer = 1;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence);
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < end)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *bytes =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(bytes);
		image.Pixels.assign(bytes, bytes + image.Width * 4);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return image;
	}
	struct GraphFile {
		std::filesystem::path Assets =
			std::filesystem::temp_directory_path() / "atomic-argument-binding-fixture";
		GraphFile(bool transform = false, bool sourceDownstream = false) {
			std::filesystem::remove_all(Assets);
			std::filesystem::create_directories(Assets / "imagegraphs");
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"argument",
				 "pc.argument",
				 "",
				 {},
				 {{"tag", std::string{"width"}}, {"type", EnumValue{1}}, {"default_value", 2.}}},
				{"dimension", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 1.0}}},
				{"solid",
				 "pc.solid",
				 "",
				 {},
				 {{"dimension", Vector2{2, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"color", Colour{31, 47, 59, 255}},
				  {"attribute_color_depth", EnumValue{3}}}}
			};
			document.Links = {
				{"argument", "value", "dimension", "x"}, {"dimension", "vector", "solid", "dimension"}
			};
			document.Outputs = {{"image", "solid", "surface_out"}};
			if (transform) {
				document.Nodes.push_back(
					{"transform",
					 sourceDownstream ? "pc.3_d_transform_image" : "image.transform_3d",
					 "",
					 {},
					 {{"position", Vector3{}},
					  {"anchor", Vector3{}},
					  {"rotation", Quaternion{}},
					  {"scale", Vector3{1, 1, 1}},
					  {"texture_tiling", Vector2{1, 1}},
					  {"projection", EnumValue{1}},
					  {"fov", 45.0},
					  {"view_range", Vector2{.001, 10}},
					  {"depth_range", Vector2{0, 1}}}}
				);
				document.Links.push_back({"solid", "surface_out", "transform", "surface"});
				document.Outputs = {{"image", "transform", "rendered"}};
				if (sourceDownstream) {
					document.Nodes.push_back({"invert", "image.invert", "", {}, {{"include_alpha", false}}});
					document.Links.push_back({"transform", "rendered", "invert", "image"});
					document.Outputs = {{"image", "invert", "image"}};
				}
			}
			std::ofstream output(client::ImageGraphDocumentPath(Assets, GRAPH));
			output << Write(document);
		}
		~GraphFile() {
			std::error_code ignored;
			std::filesystem::remove_all(Assets, ignored);
		}
	};
	void Bind(engine::ecs::Store &store) {
		engine::scene::ImageGraphBinding binding;
		binding.Graph = GRAPH;
		binding.Output = OUTPUT;
		binding.Texture = TEXTURE;
		binding.TickPolicy = engine::scene::ImageGraphTickPolicy::Fixed;
		binding.FixedTick = 0;
		REQUIRE(engine::scene::SetImageGraphBinding(store, store.Create(), binding));
	}
}

TEST_CASE(
	"Actual cached binding reads replacement arguments at the same tick before device publication",
	"[client][source_argument][argument_binding]"
) {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-idle-binding");
	Bind(store);
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	Diagnostic diagnostic;
	const std::string_view outside[] = {"width=999999"};
	REQUIRE(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic) == Status::Ok);
	const auto tick = store.Time().Tick;
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);

	const auto generation = runtime.ArgumentGeneration();
	const std::string_view invalid[] = {"width=3tail"};
	CHECK(runtime.PrepareArguments({{}, {}, invalid, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);

	const std::string_view valid[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, valid, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "image graph texture upload failed");
	CHECK(runtime.DocumentParses() == 2);
	CHECK(runtime.CachedDocumentCount() == 1);
	CHECK(store.Time().Tick == tick);
	uint32_t width = 0, height = 0;
	CHECK_FALSE(renderer.TextureSize(TEXTURE, width, height, OWNER));

	CHECK(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "image graph texture upload failed");
	CHECK(runtime.DocumentParses() == 2);

	REQUIRE(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 3);
	CHECK(store.Time().Tick == tick);
	runtime.Clear(renderer);
	CHECK(runtime.CachedDocumentCount() == 0);
}

TEST_CASE(
	"Published cached binding replaces same-tick pixels and retires the prior argument generation",
	"[client][source_argument][argument_binding][gpu][.]"
) {
	StagedShaderPaths shaders;
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-device-binding");
	Bind(store);
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	struct Video {
		~Video() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} video;
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	client::ImageGraphRuntime runtime;
	struct Retire {
		client::ImageGraphRuntime &Runtime;
		engine::render::Renderer &Renderer;
		~Retire() {
			Runtime.Clear(Renderer);
			Renderer.Shutdown();
		}
	} retire{runtime, renderer};
	const auto tick = store.Time().Tick;
	const auto path = client::ImageGraphDocumentPath(file.Assets, GRAPH);
	const auto modified = std::filesystem::last_write_time(path);
	const auto fileBytes = std::filesystem::file_size(path);
	const auto copy = [&](uint32_t expectedWidth) {
		const auto image = DownloadBinding(renderer);
		CHECK(image.Width == expectedWidth);
		CHECK(image.Height == 1);
		REQUIRE(image.Pixels.size() == expectedWidth * 4);
		for (size_t pixel = 0; pixel < expectedWidth; ++pixel) {
			CHECK(image.Pixels[pixel * 4] == std::byte{31});
			CHECK(image.Pixels[pixel * 4 + 1] == std::byte{47});
			CHECK(image.Pixels[pixel * 4 + 2] == std::byte{59});
			CHECK(image.Pixels[pixel * 4 + 3] == std::byte{255});
		}
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(2);
	CHECK(runtime.DocumentParses() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);

	Diagnostic diagnostic;
	const auto generation = runtime.ArgumentGeneration();
	const std::string_view three[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	uint32_t width = 0, height = 0;
	CHECK_FALSE(renderer.TextureSize(TEXTURE, width, height, OWNER));
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(3);
	CHECK(runtime.DocumentParses() == 2);

	const std::string_view malformed[] = {"width=4tail"};
	CHECK(runtime.PrepareArguments({{}, {}, malformed, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	copy(3);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 2);

	const std::string_view four[] = {"width=4"};
	CHECK(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	copy(3);
	REQUIRE(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(4);
	CHECK(runtime.DocumentParses() == 3);
	REQUIRE(runtime.PrepareArguments({}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(2);
	CHECK(runtime.DocumentParses() == 4);
	CHECK(store.Time().Tick == tick);
	CHECK(std::filesystem::last_write_time(path) == modified);
	CHECK(std::filesystem::file_size(path) == fileBytes);
}

TEST_CASE(
	"Argument replacement cancels actual queued binding work while stale generations cannot cancel its "
	"successor",
	"[client][source_argument][argument_binding]"
) {
	GraphFile file(true);
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-queued-binding");
	Bind(store);
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	Diagnostic diagnostic;
	const std::string_view three[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic) == Status::Ok);
	const auto generation = runtime.ArgumentGeneration();
	const auto tick = store.Time().Tick;
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.LastError().empty());
	CHECK(runtime.DocumentParses() == 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);
	const std::string_view malformed[] = {"width=4tail"};
	CHECK(runtime.PrepareArguments({{}, {}, malformed, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);

	const std::string_view four[] = {"width=4"};
	REQUIRE(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.LastError().empty());
	CHECK(runtime.DocumentParses() == 2);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));

	const std::string_view five[] = {"width=5"};
	CHECK(runtime.PrepareArguments({{}, {}, five, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 2);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));
	CHECK(renderer.CancelTransformImage3D(OWNER, TEXTURE, 2));
	CHECK(store.Time().Tick == tick);
	runtime.Clear(renderer);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 2));
}

TEST_CASE(
	"source argument replacement crosses real async Transform and downstream image binding",
	"[client][source_argument][argument-transform-gpu][gpu][.]"
) {
	StagedShaderPaths shaders;
	using Access = engine::render::test_support::TransformImage3DResidentTestAccess;
	GraphFile file(true, true);
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-transform-device");
	Bind(store);
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	struct Video {
		~Video() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} video;
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	client::ImageGraphRuntime runtime;
	struct Retire {
		client::ImageGraphRuntime &Runtime;
		engine::render::Renderer &Renderer;
		~Retire() {
			Runtime.Clear(Renderer);
			Renderer.Shutdown();
		}
	} retire{runtime, renderer};
	const auto tick = store.Time().Tick;
	const auto drive = [&](uint32_t width) {
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool published = false;
		while (std::chrono::steady_clock::now() < end) {
			Access::Poll(renderer);
			runtime.BeginFrame();
			const auto updated = runtime.Refresh(store, renderer, OWNER, file.Assets);
			INFO(runtime.LastError());
			if (updated) {
				published = true;
				break;
			}
			REQUIRE(Access::RecordAndSubmit(renderer));
			SDL_Delay(1);
		}
		INFO(runtime.LastError());
		INFO("expected width " << width);
		REQUIRE(published);
		const auto copied = DownloadBinding(renderer);
		REQUIRE(copied.Width == width);
		REQUIRE(copied.Height == 1);
		REQUIRE(copied.Pixels.size() == width * 4);
		for (size_t i = 0; i < width; ++i) {
			CHECK(copied.Pixels[i * 4] == std::byte{224});
			CHECK(copied.Pixels[i * 4 + 1] == std::byte{208});
			CHECK(copied.Pixels[i * 4 + 2] == std::byte{196});
			CHECK(copied.Pixels[i * 4 + 3] == std::byte{255});
		}
	};
	drive(2);
	Diagnostic diagnostic;
	const std::string_view three[] = {"width=3"}, four[] = {"width=4"}, bad[] = {"width=4tail"};
	REQUIRE(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	REQUIRE(Access::RecordAndSubmit(renderer));
	// Replace after submission, so the old width-three fence can complete only as cancelled work.
	REQUIRE(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic) == Status::Ok);
	drive(4);
	const auto generation = runtime.ArgumentGeneration();
	CHECK(runtime.PrepareArguments({{}, {}, bad, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	const auto retained = DownloadBinding(renderer);
	CHECK(retained.Width == 4);
	REQUIRE(runtime.PrepareArguments({}, renderer, diagnostic) == Status::Ok);
	drive(2);
	CHECK(store.Time().Tick == tick);
	runtime.Clear(renderer);
	uint32_t width = 0, height = 0;
	CHECK_FALSE(renderer.TextureSize(TEXTURE, width, height, OWNER));
}
