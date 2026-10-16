#include "GpuHeap.hpp"
#include "RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.texturecancellationgpu")
TEST_DEPENDS("engine.render.fixtures")

namespace {
	using namespace engine::render;

	constexpr uint32_t WIDTH = 17;
	constexpr uint32_t HEIGHT = 13;
	constexpr uint32_t ROW_BYTES = WIDTH * 4;
	constexpr uint32_t READBACK_ROW_BYTES = 256;
	constexpr auto FENCE_TIMEOUT = std::chrono::seconds(10);

	using Command = std::unique_ptr<SDL_GPUCommandBuffer, std::function<void(SDL_GPUCommandBuffer *)>>;
	using Fence = std::unique_ptr<SDL_GPUFence, std::function<void(SDL_GPUFence *)>>;
	using Transfer = std::unique_ptr<SDL_GPUTransferBuffer, std::function<void(SDL_GPUTransferBuffer *)>>;
	using Texture = std::unique_ptr<SDL_GPUTexture, std::function<void(SDL_GPUTexture *)>>;

	enum class CycleOperation { Upload, RenderPassClear };

	struct GpuResources {
		Texture Target{nullptr, nullptr};
		Texture Sentinel{nullptr, nullptr};

		explicit GpuResources(SDL_GPUDevice *device)
			: Target(
				  nullptr,
				  [device](SDL_GPUTexture *texture) {
					  if (texture) gpu::ReleaseTexture(device, texture);
				  }
			  ),
			  Sentinel(nullptr, [device](SDL_GPUTexture *texture) {
				  if (texture) gpu::ReleaseTexture(device, texture);
			  }) {}
	};

	Command AcquireCommand(SDL_GPUDevice *device) {
		return Command(SDL_AcquireGPUCommandBuffer(device), [](SDL_GPUCommandBuffer *command) {
			if (command) SDL_CancelGPUCommandBuffer(command);
		});
	}

	Transfer MakeTransfer(SDL_GPUDevice *device, SDL_GPUTransferBufferUsage usage, uint32_t bytes) {
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = usage;
		info.size = bytes;
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		return Transfer(transfer, [device](SDL_GPUTransferBuffer *resource) {
			if (resource) gpu::ReleaseTransferBuffer(device, resource);
		});
	}

	Texture MakeTexture(SDL_GPUDevice *device) {
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		info.width = WIDTH;
		info.height = HEIGHT;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		auto *texture = gpu::CreateTexture(device, &info);
		return Texture(texture, [device](SDL_GPUTexture *resource) {
			if (resource) gpu::ReleaseTexture(device, resource);
		});
	}

	void WaitForFence(SDL_GPUDevice *device, SDL_GPUFence *fence) {
		const auto deadline = std::chrono::steady_clock::now() + FENCE_TIMEOUT;
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		INFO(SDL_GetError());
		REQUIRE(SDL_QueryGPUFence(device, fence));
		SDL_GPUFence *fences[] = {fence};
		REQUIRE(SDL_WaitForGPUFences(device, true, fences, 1));
	}

	Fence SubmitAndWait(SDL_GPUDevice *device, Command command) {
		auto *raw = SDL_SubmitGPUCommandBufferAndAcquireFence(command.release());
		INFO(SDL_GetError());
		REQUIRE(raw != nullptr);
		Fence fence(raw, [device](SDL_GPUFence *resource) { SDL_ReleaseGPUFence(device, resource); });
		WaitForFence(device, fence.get());
		return fence;
	}

	bool Upload(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *texture,
		std::span<const std::byte> pixels,
		bool cycle
	) {
		auto transfer =
			MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<uint32_t>(pixels.size()));
		if (!transfer) return false;
		auto *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		if (!mapped) return false;
		std::memcpy(mapped, pixels.data(), pixels.size());
		SDL_UnmapGPUTransferBuffer(device, transfer.get());

		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) return false;
		const SDL_GPUTextureTransferInfo source{transfer.get(), 0, 0, 0};
		const SDL_GPUTextureRegion destination{texture, 0, 0, 0, 0, 0, WIDTH, HEIGHT, 1};
		SDL_UploadToGPUTexture(copy, &source, &destination, cycle);
		SDL_EndGPUCopyPass(copy);
		return true;
	}

	std::vector<std::byte> Readback(SDL_GPUDevice *device, SDL_GPUTexture *texture) {
		const uint32_t transferBytes = READBACK_ROW_BYTES * HEIGHT;
		auto transfer = MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, transferBytes);
		REQUIRE(transfer != nullptr);
		Command command = AcquireCommand(device);
		REQUIRE(command != nullptr);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		const SDL_GPUTextureRegion source{texture, 0, 0, 0, 0, 0, WIDTH, HEIGHT, 1};
		const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, READBACK_ROW_BYTES / 4, HEIGHT};
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		auto fence = SubmitAndWait(device, std::move(command));

		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped != nullptr);
		std::vector<std::byte> pixels(ROW_BYTES * HEIGHT);
		for (uint32_t row = 0; row < HEIGHT; ++row)
			std::memcpy(pixels.data() + row * ROW_BYTES, mapped + row * READBACK_ROW_BYTES, ROW_BYTES);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return pixels;
	}

	void
	InitialiseTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture, std::span<const std::byte> pixels) {
		Command command = AcquireCommand(device);
		REQUIRE(command != nullptr);
		REQUIRE(Upload(device, command.get(), texture, pixels, false));
		SubmitAndWait(device, std::move(command));
	}

	std::vector<std::byte> SolidPixels(std::array<uint8_t, 4> rgba) {
		std::vector<std::byte> pixels(ROW_BYTES * HEIGHT);
		for (size_t offset = 0; offset < pixels.size(); offset += rgba.size())
			for (size_t channel = 0; channel < rgba.size(); ++channel)
				pixels[offset + channel] = static_cast<std::byte>(rgba[channel]);
		return pixels;
	}

	void RunCancellation(CycleOperation operation) {
		test::FixtureDevice fixture;
		REQUIRE(fixture.VideoReady);
		REQUIRE(fixture.Render.Initialise(nullptr, 2, true));
		REQUIRE(fixture.Render.IsHeadless());
		INFO("SDL GPU backend: " << fixture.Render.BackendName());
		auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
		REQUIRE(device != nullptr);
		GpuResources resources(device);
		resources.Target = MakeTexture(device);
		resources.Sentinel = MakeTexture(device);
		REQUIRE(resources.Target != nullptr);
		REQUIRE(resources.Sentinel != nullptr);

		const auto initial = SolidPixels({12, 34, 56, 255});
		const auto replacement = SolidPixels({91, 82, 73, 255});
		const auto canceledSentinelWrite = SolidPixels({200, 1, 2, 255});
		const auto sentinelExpected = SolidPixels({7, 8, 9, 255});
		InitialiseTexture(device, resources.Target.get(), initial);
		InitialiseTexture(device, resources.Sentinel.get(), sentinelExpected);

		Transfer holderReadback(nullptr, nullptr);
		Command holder = AcquireCommand(device);
		REQUIRE(holder != nullptr);
		{
			auto transfer =
				MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, READBACK_ROW_BYTES * HEIGHT);
			REQUIRE(transfer != nullptr);
			auto *copy = SDL_BeginGPUCopyPass(holder.get());
			REQUIRE(copy != nullptr);
			const SDL_GPUTextureRegion source{resources.Target.get(), 0, 0, 0, 0, 0, WIDTH, HEIGHT, 1};
			const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, READBACK_ROW_BYTES / 4, HEIGHT};
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			holderReadback = std::move(transfer);
		}

		Command canceled = AcquireCommand(device);
		REQUIRE(canceled != nullptr);
		if (operation == CycleOperation::Upload) {
			REQUIRE(Upload(device, canceled.get(), resources.Target.get(), replacement, true));
		} else {
			SDL_GPUColorTargetInfo attachment{};
			attachment.texture = resources.Target.get();
			attachment.clear_color = {0.75f, 0.25f, 0.5f, 1.0f};
			attachment.load_op = SDL_GPU_LOADOP_CLEAR;
			attachment.store_op = SDL_GPU_STOREOP_STORE;
			attachment.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(canceled.get(), &attachment, 1, nullptr);
			REQUIRE(pass != nullptr);
			SDL_EndGPURenderPass(pass);
		}
		REQUIRE(Upload(device, canceled.get(), resources.Sentinel.get(), canceledSentinelWrite, false));
		REQUIRE(SDL_CancelGPUCommandBuffer(canceled.release()));
		REQUIRE(SDL_CancelGPUCommandBuffer(holder.release()));

		CHECK(Readback(device, resources.Target.get()).size() == ROW_BYTES * HEIGHT);
		CHECK(Readback(device, resources.Sentinel.get()) == sentinelExpected);
		Command retry = AcquireCommand(device);
		REQUIRE(retry != nullptr);
		REQUIRE(Upload(device, retry.get(), resources.Target.get(), replacement, false));
		SubmitAndWait(device, std::move(retry));
		CHECK(Readback(device, resources.Target.get()) == replacement);
	}
}

TEST_CASE(
	"canceling a cycling texture upload leaves its active backing writable", "[render][gpu][texture-cancel]"
) {
	RunCancellation(CycleOperation::Upload);
}

TEST_CASE(
	"canceling a cycling render-pass clear leaves its active backing writable",
	"[render][gpu][texture-cancel]"
) {
	RunCancellation(CycleOperation::RenderPassClear);
}
