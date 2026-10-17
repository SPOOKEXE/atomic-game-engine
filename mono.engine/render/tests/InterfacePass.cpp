#include "RenderFixture.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Float16.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

TEST_SUITE_ID("engine.render.interfacepass")
TEST_DEPENDS("engine.render.fixtures")

namespace {
	using namespace engine;
	using namespace engine::render;

	// Record the actual interface backend into an owned attachment. Only the
	// center pixel is downloaded; padded pitch still exercises SDL transfer layout.
	std::array<float, 4> InterfacePixel(
		Renderer &renderer,
		InterfacePass &interface,
		SDL_GPUTextureFormat format,
		bool spatial,
		size_t expectedBatches = 1
	) {
		constexpr uint32_t EXTENT = 8, PITCH_BYTES = 256;
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		const auto releaseTexture = [device](SDL_GPUTexture *texture) {
			gpu::ReleaseTexture(device, texture);
		};
		SDL_GPUTextureCreateInfo textureInfo{};
		textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
		textureInfo.format = format;
		textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		textureInfo.width = textureInfo.height = EXTENT;
		textureInfo.layer_count_or_depth = textureInfo.num_levels = 1;
		std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> colour(
			gpu::CreateTexture(device, &textureInfo), releaseTexture
		);
		REQUIRE(colour != nullptr);
		std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> depth(nullptr, releaseTexture);
		if (spatial) {
			textureInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			textureInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			depth.reset(gpu::CreateTexture(device, &textureInfo));
			REQUIRE(depth != nullptr);
		}
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		transferInfo.size = EXTENT * PITCH_BYTES;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
			gpu::ReleaseTransferBuffer(device, transfer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			gpu::CreateTransferBuffer(device, &transferInfo), releaseTransfer
		);
		REQUIRE(transfer != nullptr);
		const auto cancelCommand = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
			SDL_AcquireGPUCommandBuffer(device), cancelCommand
		);
		REQUIRE(command != nullptr);
		REQUIRE(interface.Prepare(command.get()));
		SDL_GPUColorTargetInfo attachment{};
		attachment.texture = colour.get();
		attachment.load_op = SDL_GPU_LOADOP_CLEAR;
		attachment.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo depthAttachment{};
		depthAttachment.texture = depth.get();
		depthAttachment.clear_depth = 1;
		depthAttachment.load_op = SDL_GPU_LOADOP_CLEAR;
		depthAttachment.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthAttachment.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthAttachment.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		auto *pass =
			SDL_BeginGPURenderPass(command.get(), &attachment, 1, spatial ? &depthAttachment : nullptr);
		REQUIRE(pass != nullptr);
		if (spatial) {
			core::CFrame camera;
			camera.Position = {0, 0, 1};
			CHECK(
				interface.RecordWorld(
					command.get(),
					pass,
					glm::mat4(1),
					camera,
					{},
					{},
					EXTENT,
					EXTENT,
					true,
					WorldColourTarget::Hdr
				) == 1
			);
		} else {
			interface.Record(command.get(), pass);
			CHECK(interface.LastBatchCount() == expectedBatches);
		}
		SDL_EndGPURenderPass(pass);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = colour.get();
		source.w = source.h = EXTENT;
		source.d = 1;
		const uint32_t pixelBytes = spatial ? 8 : 4;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = PITCH_BYTES / pixelBytes;
		destination.rows_per_layer = EXTENT;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence != nullptr);
		interface.CompleteFrame(true);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		INFO("interface pixel capture deadline");
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped != nullptr);
		std::array<std::byte, 8> pixel{};
		std::memcpy(pixel.data(), mapped + EXTENT / 2 * PITCH_BYTES + EXTENT / 2 * pixelBytes, pixelBytes);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		std::array<float, 4> result{};
		for (size_t channel = 0; channel < result.size(); channel++) {
			if (spatial) {
				const auto low = std::to_integer<uint32_t>(pixel[channel * 2]);
				const auto high = std::to_integer<uint32_t>(pixel[channel * 2 + 1]);
				result[channel] = core::DecodeFloat16(static_cast<uint16_t>(low | high << 8));
			} else
				result[channel] = std::to_integer<uint8_t>(pixel[channel]) / 255.0f;
		}
		return result;
	}
}

TEST_CASE(
	"interface sampling preserves screen bytes and spatial linear colour", "[render][gpu][interfacepass][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	assets::TextureFormat sourceFormat = assets::TextureFormat::RGBA8;
	SDL_GPUTextureFormat targetFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	bool spatial = false;
	SECTION("sRGB image into UNORM screen restores authored orange") {}
	SECTION("linear image into UNORM screen keeps raw orange") {
		sourceFormat = assets::TextureFormat::RGBA8_LINEAR;
	}
	SECTION("sRGB image into sRGB screen relies on hardware target encoding") {
		targetFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
	}
	SECTION("sRGB spatial image into HDR keeps sampled linear orange") {
		targetFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		spatial = true;
	}
	const core::Name name("interface.orange"), owner("interface.colour-owner"),
		other("interface.other-owner");
	assets::TextureData image;
	image.Width = image.Height = 1;
	image.Format = sourceFormat;
	image.Pixels = {std::byte{255}, std::byte{64}, std::byte{0}, std::byte{255}};
	REQUIRE(fixture.Render.AddTexture(name, image, owner));
	CHECK(fixture.Render.TextureSamplesSRGB(name, owner) == (sourceFormat == assets::TextureFormat::RGBA8));
	CHECK_FALSE(fixture.Render.TextureSamplesSRGB(name, other));
	CHECK(fixture.Render.TextureHandle(name, other) == nullptr);
	InterfacePass interface;
	REQUIRE(interface.Initialise(fixture.Render.Backend().Device, static_cast<uint32_t>(targetFormat)));
	interface.SetImageSource([&](const core::Name &requested) {
		CHECK(requested == name);
		InterfaceImage resolved;
		resolved.Texture = fixture.Render.TextureHandle(requested, owner);
		resolved.Width = resolved.Height = 1;
		resolved.SampledSRGB = fixture.Render.TextureSamplesSRGB(requested, owner);
		return resolved;
	});
	ecs::Store world("interface.gamma-fixture");
	gui::DrawCommand picture;
	picture.Kind = gui::DrawKind::Image;
	picture.Bounds = {{0, 0}, {8, 8}};
	picture.Clip = picture.Bounds;
	picture.Image = name;
	if (spatial) {
		gui::RegisterGuiClasses();
		const auto collector = world.CreateInstance(gui::GuiClass("SurfaceGui"), "Orange");
		gui::SpatialCanvas canvas;
		canvas.Size = {8, 8};
		canvas.Origin = {-1, 1, .5f};
		canvas.AxisX = {2, 0, 0};
		canvas.AxisY = {0, -2, 0};
		canvas.Normal = {0, 0, 1};
		canvas.AlwaysOnTop = true;
		world.Set(collector, canvas);
		picture.Collector = collector;
		picture.Spatial = true;
	}
	gui::DrawList list;
	list.Commands.push_back(picture);
	interface.Submit(list, {8, 8}, {8, 8}, world);
	const auto sampled = InterfacePixel(fixture.Render, interface, targetFormat, spatial);
	CHECK(std::abs(sampled[0] - 1) < .001f);
	CHECK(std::abs(sampled[1] - (spatial ? .05126946f : 64.0f / 255)) < (spatial ? .0001f : .5f / 255));
	CHECK(sampled[2] == 0);
	CHECK(std::abs(sampled[3] - 1) < .001f);
}

TEST_CASE(
	"retained collector ranges keep paint order and resolve live images", "[render][gpu][interfacepass][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name imageName("interface.live-range"), owner("interface.live-range-owner");
	for (const auto &[name, colour] : std::array{
			 std::pair{core::Name("interface.live-blue"), std::array<uint8_t, 4>{0, 0, 255, 255}},
			 std::pair{core::Name("interface.live-green"), std::array<uint8_t, 4>{0, 255, 0, 255}},
		 }) {
		assets::TextureData image;
		image.Width = image.Height = 1;
		image.Format = assets::TextureFormat::RGBA8;
		for (uint8_t channel : colour)
			image.Pixels.push_back(std::byte{channel});
		REQUIRE(fixture.Render.AddTexture(name, image, owner));
	}

	core::Name currentImage("interface.live-blue");
	InterfacePass interface;
	REQUIRE(interface.Initialise(fixture.Render.Backend().Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM));
	interface.SetImageSource([&](const core::Name &) {
		InterfaceImage resolved;
		resolved.Texture = fixture.Render.TextureHandle(currentImage, owner);
		resolved.Width = resolved.Height = 1;
		resolved.SampledSRGB = true;
		return resolved;
	});
	ecs::Store world("interface.live-range-fixture");
	const ecs::Entity cachedCollector(1), directCollector(2);
	gui::DrawCommand cached;
	cached.Kind = gui::DrawKind::Rectangle;
	cached.Collector = cachedCollector;
	cached.Bounds = cached.Clip = {{0, 0}, {8, 8}};
	cached.Tint = {1, 0, 0};
	gui::DrawCommand direct;
	direct.Kind = gui::DrawKind::Image;
	direct.Collector = directCollector;
	direct.Bounds = direct.Clip = {{0, 0}, {8, 8}};
	direct.Image = imageName;
	gui::DrawList list;
	list.Commands = {cached, direct};
	list.CollectorRanges = {
		{.Collector = cachedCollector, .First = 0, .Count = 1},
		{.Collector = directCollector, .First = 1, .Count = 1},
	};

	interface.Submit(list, {8, 8}, {8, 8}, world, 1, true, {});
	const auto blue =
		InterfacePixel(fixture.Render, interface, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false, 2);
	CHECK(blue[0] == 0);
	CHECK(blue[1] == 0);
	CHECK(blue[2] == 1);

	currentImage = core::Name("interface.live-green");
	interface.Submit(list, {8, 8}, {8, 8}, world, 1, true, {});
	const auto green =
		InterfacePixel(fixture.Render, interface, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false, 2);
	CHECK(green[0] == 0);
	CHECK(green[1] == 1);
	CHECK(green[2] == 0);
}

TEST_CASE("canvas group target bounds are clipped before device sizing", "[render][interfacepass]") {
	const auto target = InterfaceGroupTargetFor(
		{{20.0f, -10.0f}, {180.0f, 70.0f}},
		{{40.0f, 10.0f}, {300.0f, 90.0f}},
		{100.0f, 80.0f},
		{200.0f, 160.0f}
	);
	REQUIRE(target.has_value());
	CHECK(target->Bounds.Min.X == 40.0f);
	CHECK(target->Bounds.Min.Y == 10.0f);
	CHECK(target->Bounds.Max.X == 100.0f);
	CHECK(target->Bounds.Max.Y == 70.0f);
	CHECK(target->Width == 120);
	CHECK(target->Height == 120);
}

TEST_CASE("canvas group target sizing refuses hostile coordinates", "[render][interfacepass]") {
	const core::Rect canvas{{0.0f, 0.0f}, {100.0f, 80.0f}};
	const core::Vector2 targetPixels{200.0f, 160.0f};
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{std::numeric_limits<float>::quiet_NaN(), 0.0f}, {10.0f, 10.0f}}, canvas, canvas.Max, targetPixels
	));
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {std::numeric_limits<float>::infinity(), 10.0f}}, canvas, canvas.Max, targetPixels
	));
	CHECK_FALSE(
		InterfaceGroupTargetFor({{-20.0f, -20.0f}, {-1.0f, -1.0f}}, canvas, canvas.Max, targetPixels)
	);
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {10.0f, 10.0f}}, canvas, canvas.Max, {std::numeric_limits<float>::max(), 160.0f}
	));
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {100.0f, 80.0f}},
		canvas,
		canvas.Max,
		{static_cast<float>(MAXIMUM_INTERFACE_GROUP_TARGET_EDGE + 1),
		 static_cast<float>(MAXIMUM_INTERFACE_GROUP_TARGET_EDGE + 1)}
	));

	const auto clamped =
		InterfaceGroupTargetFor({{-1.0e30f, -1.0e30f}, {1.0e30f, 1.0e30f}}, canvas, canvas.Max, targetPixels);
	REQUIRE(clamped.has_value());
	CHECK(clamped->Bounds == canvas);
	CHECK(clamped->Width == 200);
	CHECK(clamped->Height == 160);
}
