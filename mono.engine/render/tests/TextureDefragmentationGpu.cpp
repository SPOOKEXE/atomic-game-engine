#include "GpuHeap.hpp"

#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.texturedefragmentationgpu")
TEST_DEPENDS("engine.render.shadercompiler")

namespace {
	using namespace engine::render;

	constexpr uint32_t SOURCE_EXTENT = 64;
	constexpr uint32_t SOURCE_LEVELS = 7;
	constexpr uint32_t OUTPUT_EXTENT = 1024;
	constexpr uint32_t OUTPUT_SAMPLES = SOURCE_LEVELS;
	constexpr uint32_t TRANSFER_ROW_PIXELS = 64;
	constexpr uint32_t TRANSFER_ROW_BYTES = TRANSFER_ROW_PIXELS * 4;
	constexpr auto FENCE_TIMEOUT = std::chrono::seconds(10);

	constexpr std::array<std::array<uint8_t, 4>, SOURCE_LEVELS> MIP_COLOURS{{
		{0, 0, 0, 255},
		{255, 0, 0, 255},
		{0, 255, 0, 255},
		{0, 0, 255, 255},
		{255, 255, 0, 255},
		{255, 0, 255, 255},
		{0, 255, 255, 255},
	}};

	using Command = std::unique_ptr<SDL_GPUCommandBuffer, std::function<void(SDL_GPUCommandBuffer *)>>;
	using Fence = std::unique_ptr<SDL_GPUFence, std::function<void(SDL_GPUFence *)>>;
	using Texture = std::unique_ptr<SDL_GPUTexture, std::function<void(SDL_GPUTexture *)>>;
	using Transfer = std::unique_ptr<SDL_GPUTransferBuffer, std::function<void(SDL_GPUTransferBuffer *)>>;
	using Shader = std::unique_ptr<SDL_GPUShader, std::function<void(SDL_GPUShader *)>>;
	using Sampler = std::unique_ptr<SDL_GPUSampler, std::function<void(SDL_GPUSampler *)>>;
	using Pipeline = std::unique_ptr<SDL_GPUGraphicsPipeline, std::function<void(SDL_GPUGraphicsPipeline *)>>;

	struct SDLFixture {
		bool VideoReady = SDL_Init(SDL_INIT_VIDEO);
		SDL_GPUDevice *Device = nullptr;

		~SDLFixture() {
			if (Device != nullptr) {
				gpu::ForgetDevice(Device);
				SDL_DestroyGPUDevice(Device);
			}
			if (VideoReady) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}

		void Initialise() {
			REQUIRE(VideoReady);
			if (!SDL_GPUSupportsShaderFormats(SDL_GPU_SHADERFORMAT_SPIRV, "vulkan")) {
				SKIP("SDL Vulkan with SPIR-V is unavailable.");
			}
			Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
			INFO(SDL_GetError());
			REQUIRE(Device != nullptr);
		}
	};

	Command AcquireCommand(SDL_GPUDevice *device) {
		return Command(SDL_AcquireGPUCommandBuffer(device), [](SDL_GPUCommandBuffer *command) {
			if (command != nullptr) SDL_CancelGPUCommandBuffer(command);
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
		auto *rawFence = SDL_SubmitGPUCommandBufferAndAcquireFence(command.release());
		INFO(SDL_GetError());
		REQUIRE(rawFence != nullptr);
		Fence fence(rawFence, [device](SDL_GPUFence *resource) { SDL_ReleaseGPUFence(device, resource); });
		WaitForFence(device, fence.get());
		return fence;
	}

	void SubmitEmptyAndWait(SDL_GPUDevice *device) {
		Command command = AcquireCommand(device);
		REQUIRE(command != nullptr);
		SubmitAndWait(device, std::move(command));
	}

	Texture
	MakeTexture(SDL_GPUDevice *device, uint32_t width, uint32_t height, uint32_t levels, uint32_t usage) {
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = usage;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = levels;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		auto *texture = gpu::CreateTexture(device, &info);
		return Texture(texture, [device](SDL_GPUTexture *resource) {
			if (resource != nullptr) gpu::ReleaseTexture(device, resource);
		});
	}

	Transfer MakeTransfer(SDL_GPUDevice *device, SDL_GPUTransferBufferUsage usage, uint32_t size) {
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = usage;
		info.size = size;
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		return Transfer(transfer, [device](SDL_GPUTransferBuffer *resource) {
			if (resource != nullptr) gpu::ReleaseTransferBuffer(device, resource);
		});
	}

	uint32_t MipOffset(uint32_t mipLevel) {
		uint32_t offset = 0;
		for (uint32_t level = 0; level < mipLevel; ++level)
			offset += TRANSFER_ROW_BYTES * (SOURCE_EXTENT >> level);
		return offset;
	}

	std::vector<std::byte> MipUploadBytes() {
		std::vector<std::byte> bytes(MipOffset(SOURCE_LEVELS), std::byte{0});
		for (uint32_t level = 0; level < SOURCE_LEVELS; ++level) {
			const uint32_t extent = SOURCE_EXTENT >> level;
			const size_t levelOffset = MipOffset(level);
			for (uint32_t y = 0; y < extent; ++y) {
				for (uint32_t x = 0; x < extent; ++x) {
					const size_t pixelOffset = levelOffset + size_t(y) * TRANSFER_ROW_BYTES + size_t(x) * 4;
					for (size_t channel = 0; channel < 4; ++channel)
						bytes[pixelOffset + channel] = std::byte{MIP_COLOURS[level][channel]};
				}
			}
		}
		return bytes;
	}

	void UploadMipChain(SDL_GPUDevice *device, SDL_GPUTexture *texture) {
		const auto bytes = MipUploadBytes();
		auto transfer =
			MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<uint32_t>(bytes.size()));
		REQUIRE(transfer != nullptr);
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped != nullptr);
		std::memcpy(mapped, bytes.data(), bytes.size());
		SDL_UnmapGPUTransferBuffer(device, transfer.get());

		Command command = AcquireCommand(device);
		REQUIRE(command != nullptr);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		for (uint32_t level = 0; level < SOURCE_LEVELS; ++level) {
			const uint32_t extent = SOURCE_EXTENT >> level;
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = transfer.get();
			source.offset = MipOffset(level);
			source.pixels_per_row = TRANSFER_ROW_PIXELS;
			source.rows_per_layer = extent;
			SDL_GPUTextureRegion destination{};
			destination.texture = texture;
			destination.mip_level = level;
			destination.w = extent;
			destination.h = extent;
			destination.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
		}
		SDL_EndGPUCopyPass(copy);
		SubmitAndWait(device, std::move(command));
	}

	std::vector<uint32_t>
	CompileShader(ShaderCompiler &compiler, std::string_view source, ShaderStage stage) {
		const ShaderCompilation compiled = compiler.Compile(source, stage);
		REQUIRE_FALSE(compiled.Failed);
		REQUIRE_FALSE(compiled.SpirV.empty());
		return compiled.SpirV;
	}

	SDL_GPUShader *
	CreateShader(SDL_GPUDevice *device, const std::vector<uint32_t> &spirv, SDL_GPUShaderStage stage) {
		SDL_GPUShaderCreateInfo info{};
		info.code = reinterpret_cast<const uint8_t *>(spirv.data());
		info.code_size = spirv.size() * sizeof(uint32_t);
		info.entrypoint = "main";
		info.format = SDL_GPU_SHADERFORMAT_SPIRV;
		info.stage = stage;
		info.num_samplers = stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
		return SDL_CreateGPUShader(device, &info);
	}

	Pipeline MakePipeline(SDL_GPUDevice *device) {
		ShaderCompiler compiler;
		const std::vector<uint32_t> vertexSpirv = CompileShader(
			compiler,
			R"(#version 450
void main() {
	vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
})",
			ShaderStage::Vertex
		);
		const std::vector<uint32_t> fragmentSpirv = CompileShader(
			compiler,
			R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D sourceTexture;
layout(location = 0) out vec4 outputColour;
void main() {
	outputColour = textureLod(sourceTexture, vec2(0.5), floor(gl_FragCoord.x));
})",
			ShaderStage::Fragment
		);
		const auto releaseShader = [device](SDL_GPUShader *resource) {
			if (resource != nullptr) SDL_ReleaseGPUShader(device, resource);
		};
		Shader vertex(CreateShader(device, vertexSpirv, SDL_GPU_SHADERSTAGE_VERTEX), releaseShader);
		Shader fragment(CreateShader(device, fragmentSpirv, SDL_GPU_SHADERSTAGE_FRAGMENT), releaseShader);
		REQUIRE(vertex != nullptr);
		REQUIRE(fragment != nullptr);
		SDL_GPUColorTargetDescription colourTarget{};
		colourTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex.get();
		info.fragment_shader = fragment.get();
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		info.target_info.color_target_descriptions = &colourTarget;
		info.target_info.num_color_targets = 1;
		auto *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		INFO(SDL_GetError());
		REQUIRE(pipeline != nullptr);
		return Pipeline(pipeline, [device](SDL_GPUGraphicsPipeline *resource) {
			if (resource != nullptr) SDL_ReleaseGPUGraphicsPipeline(device, resource);
		});
	}

	Sampler MakeSampler(SDL_GPUDevice *device) {
		SDL_GPUSamplerCreateInfo info{};
		info.min_filter = SDL_GPU_FILTER_NEAREST;
		info.mag_filter = SDL_GPU_FILTER_NEAREST;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		info.min_lod = 0.0f;
		info.max_lod = static_cast<float>(SOURCE_LEVELS - 1);
		auto *sampler = SDL_CreateGPUSampler(device, &info);
		return Sampler(sampler, [device](SDL_GPUSampler *resource) {
			if (resource != nullptr) SDL_ReleaseGPUSampler(device, resource);
		});
	}

	void RecordRetainedDraw(
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *output,
		SDL_GPUTexture *source,
		SDL_GPUSampler *sampler,
		SDL_GPUGraphicsPipeline *pipeline
	) {
		SDL_GPUColorTargetInfo target{};
		target.texture = output;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
		auto *pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
		REQUIRE(pass != nullptr);
		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		const SDL_GPUTextureSamplerBinding binding{source, sampler};
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		const SDL_Rect viewport{0, 0, static_cast<int>(OUTPUT_SAMPLES), 1};
		const SDL_GPUViewport viewportState{0.0f, 0.0f, float(OUTPUT_SAMPLES), 1.0f, 0.0f, 1.0f};
		SDL_SetGPUViewport(pass, &viewportState);
		SDL_SetGPUScissor(pass, &viewport);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}

	std::vector<std::byte> ReadTexture(
		SDL_GPUDevice *device, SDL_GPUTexture *texture, uint32_t width, uint32_t height, uint32_t mipLevel = 0
	) {
		const uint32_t rows = height;
		const uint32_t transferSize = TRANSFER_ROW_BYTES * rows;
		auto transfer = MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, transferSize);
		REQUIRE(transfer != nullptr);
		Command command = AcquireCommand(device);
		REQUIRE(command != nullptr);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.mip_level = mipLevel;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = TRANSFER_ROW_PIXELS;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		SubmitAndWait(device, std::move(command));
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped != nullptr);
		std::vector<std::byte> bytes(size_t(width) * height * 4);
		for (uint32_t row = 0; row < height; ++row)
			std::memcpy(
				bytes.data() + size_t(row) * width * 4, mapped + size_t(row) * TRANSFER_ROW_BYTES, width * 4
			);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return bytes;
	}

	bool PixelsMatch(
		std::span<const std::byte> pixels,
		uint32_t width,
		uint32_t height,
		const std::array<uint8_t, 4> &colour
	) {
		if (pixels.size() != size_t(width) * height * 4) return false;
		for (size_t offset = 0; offset < pixels.size(); offset += colour.size())
			for (size_t channel = 0; channel < colour.size(); ++channel)
				if (pixels[offset + channel] != std::byte{colour[channel]}) return false;
		return true;
	}

	void RunDefragmentationReadback() {
		SDLFixture fixture;
		fixture.Initialise();
		auto *device = fixture.Device;

		auto source =
			MakeTexture(device, SOURCE_EXTENT, SOURCE_EXTENT, SOURCE_LEVELS, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		REQUIRE(source != nullptr);
		UploadMipChain(device, source.get());

		std::vector<Texture> fillers;
		fillers.reserve(15);
		for (uint32_t index = 0; index < 15; ++index) {
			auto filler = MakeTexture(device, 512, 512, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
			REQUIRE(filler != nullptr);
			fillers.push_back(std::move(filler));
		}

		// This 4 MiB target uses SDL's large pool; pin only it so small-pool source compaction stays
		// eligible.
		auto output = MakeTexture(device, OUTPUT_EXTENT, OUTPUT_EXTENT, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		REQUIRE(output != nullptr);
		auto pipeline = MakePipeline(device);
		auto sampler = MakeSampler(device);
		REQUIRE(pipeline != nullptr);
		REQUIRE(sampler != nullptr);

		auto outputPinTransfer =
			MakeTransfer(device, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, TRANSFER_ROW_BYTES);
		REQUIRE(outputPinTransfer != nullptr);
		Command outputPin = AcquireCommand(device);
		REQUIRE(outputPin != nullptr);
		auto *pinPass = SDL_BeginGPUCopyPass(outputPin.get());
		REQUIRE(pinPass != nullptr);
		SDL_GPUTextureRegion pinSource{};
		pinSource.texture = output.get();
		pinSource.w = pinSource.h = pinSource.d = 1;
		SDL_GPUTextureTransferInfo pinDestination{};
		pinDestination.transfer_buffer = outputPinTransfer.get();
		pinDestination.pixels_per_row = TRANSFER_ROW_PIXELS;
		pinDestination.rows_per_layer = 1;
		SDL_DownloadFromGPUTexture(pinPass, &pinSource, &pinDestination);
		SDL_EndGPUCopyPass(pinPass);

		Command retainedDraw = AcquireCommand(device);
		REQUIRE(retainedDraw != nullptr);
		RecordRetainedDraw(retainedDraw.get(), output.get(), source.get(), sampler.get(), pipeline.get());

		// The last live filler isolates the tail from 1 MiB holes; SDL's pool policy decides whether 1.5 MiB
		// compacts.
		for (size_t index = 0; index + 1 < fillers.size(); index += 2)
			fillers[index].reset();
		SubmitEmptyAndWait(device);

		auto defragmentTrigger = MakeTexture(device, 768, 512, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		REQUIRE(defragmentTrigger != nullptr);
		SubmitEmptyAndWait(device);

		SubmitAndWait(device, std::move(retainedDraw));
		REQUIRE(SDL_CancelGPUCommandBuffer(outputPin.release()));

		const auto sampled = ReadTexture(device, output.get(), OUTPUT_SAMPLES, 1);
		for (uint32_t pixel = 0; pixel < OUTPUT_SAMPLES; ++pixel) {
			const auto actual = std::span(sampled).subspan(size_t(pixel) * 4, 4);
			INFO("sampled mip " << pixel);
			CHECK(PixelsMatch(actual, 1, 1, MIP_COLOURS[pixel]));
		}

		for (uint32_t level = 0; level < SOURCE_LEVELS; ++level) {
			const uint32_t extent = SOURCE_EXTENT >> level;
			const auto mipPixels = ReadTexture(device, source.get(), extent, extent, level);
			INFO("source mip " << level);
			CHECK(PixelsMatch(mipPixels, extent, extent, MIP_COLOURS[level]));
		}
	}
}

TEST_CASE(
	"SDL texture defragmentation preserves queued sampled mip contents", "[render][gpu][texture-defrag]"
) {
	RunDefragmentationReadback();
}
