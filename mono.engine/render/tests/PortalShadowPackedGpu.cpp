#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"

#include <engine/render/PortalShadowPacked.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

TEST_SUITE_ID("engine.render.portalshadowpackedgpu")
TEST_DEPENDS("engine.render.portalshadowpacked")

namespace {
	using namespace engine;
	using namespace engine::render;

	template <auto Release, class T> auto Owned(SDL_GPUDevice *device, T *value) {
		const auto release = [device](T *resource) { Release(device, resource); };
		return std::unique_ptr<T, decltype(release)>(value, release);
	}

	auto Shader(SDL_GPUDevice *device, const char *name, SDL_GPUShaderStage stage, uint32_t buffers) {
		const auto binary = ShaderBinaryFor(device);
		std::ifstream file(resources::Shader(name, binary.Form), std::ios::binary | std::ios::ate);
		REQUIRE(file);
		const auto length = file.tellg();
		REQUIRE(length > 0);
		std::vector<uint8_t> code(static_cast<size_t>(length));
		file.seekg(0);
		file.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(code.size()));
		REQUIRE(file);
		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = binary.EntryPoint;
		info.format = binary.Format;
		info.stage = stage;
		info.num_storage_buffers = buffers;
		auto shader = Owned<SDL_ReleaseGPUShader>(device, SDL_CreateGPUShader(device, &info));
		REQUIRE(shader);
		return shader;
	}
}

TEST_CASE("packed shadow shader reconstructs exact native D32 bits", "[render][gpu][shadow-packed][.]") {
	const int pattern = GENERATE(0, 1, 2);
	CAPTURE(pattern);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	constexpr uint32_t extent = 2048;
	constexpr uint32_t samples = extent * extent;
	std::vector<std::byte> depth(size_t(samples) * sizeof(float));
	for (uint32_t index = 0; index < samples; ++index) {
		uint32_t bits = std::bit_cast<uint32_t>(.375f);
		if (pattern == 1) bits = std::bit_cast<uint32_t>(float(index) / float(samples - 1));
		if (pattern == 2) {
			const uint32_t width = (index / 64) % 25;
			const uint32_t mask = (1u << width) - 1;
			bits = 0x3e000000u + ((index * 2654435761u) & mask);
			// Wide blocks exercise 30-bit deltas without generating denormals.
			if ((index / 64) % 31 == 0) {
				constexpr std::array<uint32_t, 4> wide{0, 0x3e800000u, 0x3f000000u, 0x3f800000u};
				bits = wide[index % wide.size()];
			}
		}
		for (size_t byte = 0; byte < 4; ++byte)
			depth[size_t(index) * 4 + byte] = std::byte(bits >> (8 * byte));
	}
	std::vector<uint32_t> words;
	std::string error;
	REQUIRE(PackPortalShadow(depth, depth.size(), words, error));
	REQUIRE(words.size() * sizeof(uint32_t) <= depth.size());
	const auto vertex = Shader(device, "overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
	const auto fragment = Shader(device, "shadow-unpack.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
	SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.vertex_shader = vertex.get();
	pipelineInfo.fragment_shader = fragment.get();
	pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	pipelineInfo.depth_stencil_state.enable_depth_test = true;
	pipelineInfo.depth_stencil_state.enable_depth_write = true;
	pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
	pipelineInfo.target_info.has_depth_stencil_target = true;
	pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	auto pipeline =
		Owned<SDL_ReleaseGPUGraphicsPipeline>(device, SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo));
	REQUIRE(pipeline);
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	bufferInfo.size = static_cast<uint32_t>(words.size() * sizeof(uint32_t));
	auto packed = Owned<gpu::ReleaseBuffer>(device, gpu::CreateBuffer(device, &bufferInfo));
	REQUIRE(packed);
	SDL_GPUTextureCreateInfo textureInfo{};
	textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
	textureInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	textureInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	textureInfo.width = textureInfo.height = extent;
	textureInfo.layer_count_or_depth = textureInfo.num_levels = 1;
	textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
	auto texture = Owned<gpu::ReleaseTexture>(device, gpu::CreateTexture(device, &textureInfo));
	REQUIRE(texture);
	SDL_GPUTransferBufferCreateInfo transferInfo{};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferInfo.size = bufferInfo.size;
	auto upload = Owned<gpu::ReleaseTransferBuffer>(device, gpu::CreateTransferBuffer(device, &transferInfo));
	REQUIRE(upload);
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transferInfo.size = static_cast<uint32_t>(depth.size());
	auto download =
		Owned<gpu::ReleaseTransferBuffer>(device, gpu::CreateTransferBuffer(device, &transferInfo));
	REQUIRE(download);
	auto *mapped = SDL_MapGPUTransferBuffer(device, upload.get(), false);
	REQUIRE(mapped);
	std::memcpy(mapped, words.data(), bufferInfo.size);
	SDL_UnmapGPUTransferBuffer(device, upload.get());
	const auto cancel = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
	std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> command(
		SDL_AcquireGPUCommandBuffer(device), cancel
	);
	REQUIRE(command);
	auto *copy = SDL_BeginGPUCopyPass(command.get());
	REQUIRE(copy);
	SDL_GPUTransferBufferLocation from{upload.get(), 0};
	SDL_GPUBufferRegion to{packed.get(), 0, bufferInfo.size};
	SDL_UploadToGPUBuffer(copy, &from, &to, false);
	SDL_EndGPUCopyPass(copy);
	SDL_GPUDepthStencilTargetInfo target{};
	target.texture = texture.get();
	target.clear_depth = 1;
	target.load_op = SDL_GPU_LOADOP_CLEAR;
	target.store_op = SDL_GPU_STOREOP_STORE;
	target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	auto *pass = SDL_BeginGPURenderPass(command.get(), nullptr, 0, &target);
	REQUIRE(pass);
	SDL_BindGPUGraphicsPipeline(pass, pipeline.get());
	auto *storage = packed.get();
	SDL_BindGPUFragmentStorageBuffers(pass, 0, &storage, 1);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	copy = SDL_BeginGPUCopyPass(command.get());
	REQUIRE(copy);
	SDL_GPUTextureRegion region{};
	region.texture = texture.get();
	region.w = region.h = extent;
	region.d = 1;
	SDL_GPUTextureTransferInfo output{};
	output.transfer_buffer = download.get();
	output.pixels_per_row = output.rows_per_layer = extent;
	SDL_DownloadFromGPUTexture(copy, &region, &output);
	SDL_EndGPUCopyPass(copy);
	auto fence =
		Owned<SDL_ReleaseGPUFence>(device, SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()));
	REQUIRE(fence);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
		SDL_Delay(1);
	REQUIRE(SDL_QueryGPUFence(device, fence.get()));
	const auto *result =
		static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, download.get(), false));
	REQUIRE(result);
	const auto mismatch = std::mismatch(depth.begin(), depth.end(), result);
	const size_t firstMismatch = static_cast<size_t>(mismatch.first - depth.begin());
	SDL_UnmapGPUTransferBuffer(device, download.get());
	CAPTURE(firstMismatch);
	CHECK(firstMismatch == depth.size());
}
