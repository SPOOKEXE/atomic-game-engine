#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherTemporal.hpp"

#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.render.weathertemporalgpu")

namespace {
	using namespace engine;
	using namespace engine::render;

	SDL_GPUComputePipeline *Pipeline(SDL_GPUDevice *device, const char *name) {
		const auto binary = ShaderBinaryFor(device);
		std::ifstream file(resources::Shader(name, binary.Form), std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		REQUIRE(!bytes.empty());
		SDL_GPUComputePipelineCreateInfo info{};
		info.code = reinterpret_cast<const Uint8 *>(bytes.data());
		info.code_size = bytes.size();
		info.entrypoint = binary.EntryPoint;
		info.format = binary.Format;
		info.num_samplers = 4;
		info.num_readwrite_storage_textures = 1;
		info.num_uniform_buffers = 1;
		info.threadcount_x = 8;
		info.threadcount_y = 8;
		info.threadcount_z = 1;
		return SDL_CreateGPUComputePipeline(device, &info);
	}

	SDL_GPUTexture *Texture(SDL_GPUDevice *device, uint32_t width, uint32_t height) {
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
					 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		return gpu::CreateTexture(device, &info);
	}

	void Clear(SDL_GPUCommandBuffer *command, SDL_GPUTexture *texture, SDL_FColor colour) {
		SDL_GPUColorTargetInfo target{};
		target.texture = texture;
		target.clear_color = colour;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.cycle = true;
		auto *pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
		REQUIRE(pass);
		SDL_EndGPURenderPass(pass);
	}

	float Half(uint16_t bits) {
		const uint32_t exponent = (bits >> 10) & 31;
		const uint32_t fraction = bits & 1023;
		if (exponent == 0) return std::ldexp(float(fraction), -24) * ((bits & 32768) ? -1 : 1);
		const uint32_t wide = ((uint32_t(bits) & 32768) << 16) |
							  ((exponent == 31 ? 255 : exponent + 112) << 23) | (fraction << 13);
		return std::bit_cast<float>(wide);
	}

	void CloudImpulse(SDL_GPUDevice *device, SDL_GPUCommandBuffer *command, SDL_GPUTexture *clouds) {
		// Exact binary16 values: black in the first cell, .5 elsewhere, all with .5 transmittance.
		const std::array<uint16_t, 16> words{
			0,
			0,
			0,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800,
			0x3800
		};
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = sizeof(words);
		const auto release = [device](SDL_GPUTransferBuffer *buffer) {
			gpu::ReleaseTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
			gpu::CreateTransferBuffer(device, &info), release
		);
		REQUIRE(transfer);
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped);
		std::memcpy(mapped, words.data(), sizeof(words));
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureTransferInfo source{};
		source.transfer_buffer = transfer.get();
		source.pixels_per_row = 2;
		source.rows_per_layer = 2;
		SDL_GPUTextureRegion target{};
		target.texture = clouds;
		target.w = target.h = 2;
		target.d = 1;
		SDL_UploadToGPUTexture(copy, &source, &target, true);
		SDL_EndGPUCopyPass(copy);
	}

	std::array<float, 4> Pixel(SDL_GPUDevice *device, SDL_GPUTexture *texture) {
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = 8;
		const auto release = [device](SDL_GPUTransferBuffer *buffer) {
			gpu::ReleaseTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
			gpu::CreateTransferBuffer(device, &info), release
		);
		REQUIRE(transfer);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = source.h = source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		const bool complete = SDL_QueryGPUFence(device, fence);
		SDL_ReleaseGPUFence(device, fence);
		REQUIRE(complete);
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped);
		std::array<uint16_t, 4> words{};
		std::memcpy(words.data(), mapped, sizeof(words));
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return {Half(words[0]), Half(words[1]), Half(words[2]), Half(words[3])};
	}
}

TEST_CASE(
	"GPU weather reconstruction keeps opacity and rejects depth silhouettes",
	"[render][gpu][weather][temporal][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releaseTexture = [device](SDL_GPUTexture *texture) { gpu::ReleaseTexture(device, texture); };
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> temporal(
		Pipeline(device, "weather-temporal.comp"), releasePipeline
	);
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> reconstruction(
		Pipeline(device, "weather-reconstruct.comp"), releasePipeline
	);
	REQUIRE(temporal);
	REQUIRE(reconstruction);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> source(Texture(device, 4, 4), releaseTexture);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> depth(Texture(device, 4, 4), releaseTexture);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> clouds(Texture(device, 2, 2), releaseTexture);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> cloudDepth(
		Texture(device, 2, 2), releaseTexture
	);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> resolved(Texture(device, 2, 2), releaseTexture);
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> output(Texture(device, 4, 4), releaseTexture);
	REQUIRE(source);
	REQUIRE(depth);
	REQUIRE(clouds);
	REQUIRE(cloudDepth);
	REQUIRE(resolved);
	REQUIRE(output);
	SDL_GPUSamplerCreateInfo samplerInfo{};
	const auto releaseSampler = [device](SDL_GPUSampler *sampler) { SDL_ReleaseGPUSampler(device, sampler); };
	std::unique_ptr<SDL_GPUSampler, decltype(releaseSampler)> sampler(
		SDL_CreateGPUSampler(device, &samplerInfo), releaseSampler
	);
	REQUIRE(sampler);
	WeatherTemporalHistory history;
	REQUIRE(EnsureWeatherTemporalHistory(device, history, 2, 2));
	struct HistoryLifetime {
		SDL_GPUDevice *Device;
		WeatherTemporalHistory &History;
		~HistoryLifetime() {
			ReleaseWeatherTemporalHistory(Device, History);
		}
	} historyLifetime{device, history};
	WeatherReconstructionUniforms uniforms;
	uniforms.Limits.x = 0;
	uniforms.Extents = {4, 4, 2, 2};
	for (const bool matchingDepth : {false, true}) {
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		Clear(command, source.get(), {.75f, .5f, .25f, 1});
		Clear(command, depth.get(), {1, 0, 0, 1});
		Clear(command, clouds.get(), {.125f, .25f, .375f, .5f});
		Clear(command, cloudDepth.get(), {matchingDepth ? 1.0f : 0.0f, 1000, 500, 1});
		REQUIRE(RecordWeatherReconstruction(
			command,
			temporal.get(),
			reconstruction.get(),
			source.get(),
			depth.get(),
			clouds.get(),
			cloudDepth.get(),
			resolved.get(),
			output.get(),
			history,
			sampler.get(),
			uniforms
		));
		history.Stage({}, command);
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		history.Commit(command);
		const auto pixel = Pixel(device, output.get());
		const std::array expected =
			matchingDepth ? std::array{.5f, .5f, .5f, 1.0f} : std::array{.75f, .5f, .25f, 1.0f};
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(pixel[channel] == Catch::Approx(expected[channel]).margin(.001));
	}
	uniforms.PreviousViewProjection = glm::perspectiveLH_ZO(1.0f, 1.0f, 1.0f, 5000.0f);
	uniforms.InverseViewProjection = glm::inverse(uniforms.PreviousViewProjection);
	uniforms.Limits.x = .85f;
	for (const bool disoccluded : {false, true}) {
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		Clear(command, history.Clouds, {.25f, .25f, .25f, .5f});
		Clear(command, history.Depth, {disoccluded ? 0.0f : 1.0f, 1000, 500, 1});
		CloudImpulse(device, command, clouds.get());
		history.Ready = true;
		REQUIRE(RecordWeatherReconstruction(
			command,
			temporal.get(),
			reconstruction.get(),
			source.get(),
			depth.get(),
			clouds.get(),
			cloudDepth.get(),
			resolved.get(),
			output.get(),
			history,
			sampler.get(),
			uniforms
		));
		history.Stage({}, command);
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		history.Commit(command);
		const auto pixel = Pixel(device, output.get());
		const float accumulated = disoccluded ? 0.0f : .25f * .85f;
		CHECK(pixel[0] == Catch::Approx(.75f * .5f + accumulated).margin(.002));
		CHECK(pixel[1] == Catch::Approx(.5f * .5f + accumulated).margin(.002));
		CHECK(pixel[2] == Catch::Approx(.25f * .5f + accumulated).margin(.002));
		CHECK(pixel[3] == 1.0f);
	}
}
