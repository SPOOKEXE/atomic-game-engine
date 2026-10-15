// Real-device coverage of the volume consumer across storage and topology changes.

#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherGpu.hpp"

#include <engine/resources/Shaders.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <glm/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.weathercloudseams")
TEST_DEPENDS("engine.render.weathergpu")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 33;
	constexpr uint32_t ROW_BYTES = 512;
	constexpr uint32_t FIELD_BYTES = 33 * 17 * 33 * 8;
	constexpr float HALF_ALLOWANCE = .001f;
	using Image = std::vector<glm::vec4>;

	struct CloudDevice {
		SDL_GPUDevice *Device;
		std::array<SDL_GPUComputePipeline *, 3> Pipelines{};
		std::array<SDL_GPUBuffer *, 4> Buffers{};
		std::array<SDL_GPUTexture *, 3> Textures{};
		SDL_GPUSampler *Sampler = nullptr;

		~CloudDevice() {
			for (auto *pipeline : Pipelines)
				if (pipeline) SDL_ReleaseGPUComputePipeline(Device, pipeline);
			for (auto *buffer : Buffers)
				if (buffer) gpu::ReleaseBuffer(Device, buffer);
			for (auto *texture : Textures)
				if (texture) gpu::ReleaseTexture(Device, texture);
			if (Sampler) SDL_ReleaseGPUSampler(Device, Sampler);
		}

		SDL_GPUComputePipeline *Pipeline(
			const char *name,
			uint32_t samplers,
			uint32_t reads,
			uint32_t textureWrites,
			uint32_t bufferWrites,
			uint32_t threadsX,
			uint32_t threadsY
		) {
			const auto binary = ShaderBinaryFor(Device);
			std::ifstream file(resources::Shader(name, binary.Form), std::ios::binary);
			const std::vector<char> bytes{
				std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()
			};
			REQUIRE_FALSE(bytes.empty());
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = reinterpret_cast<const Uint8 *>(bytes.data());
			info.code_size = bytes.size();
			info.entrypoint = binary.EntryPoint;
			info.format = binary.Format;
			info.num_samplers = samplers;
			info.num_readonly_storage_buffers = reads;
			info.num_readwrite_storage_textures = textureWrites;
			info.num_readwrite_storage_buffers = bufferWrites;
			info.num_uniform_buffers = 1;
			info.threadcount_x = threadsX;
			info.threadcount_y = threadsY;
			info.threadcount_z = 1;
			return SDL_CreateGPUComputePipeline(Device, &info);
		}

		void Initialise() {
			Pipelines[0] = Pipeline("weather-generate.comp", 0, 2, 0, 1, 64, 1);
			Pipelines[1] = Pipeline("weather-summary.comp", 0, 3, 0, 1, 64, 1);
			Pipelines[2] = Pipeline("weather-clouds.comp", 1, 5, 2, 0, 8, 8);
			for (auto *pipeline : Pipelines)
				REQUIRE(pipeline);
			const std::array<uint32_t, 4> sizes{FIELD_BYTES, 128 * 16, 64 * 16, 64 * 16};
			for (size_t index = 0; index < Buffers.size(); ++index) {
				SDL_GPUBufferCreateInfo info{};
				info.usage =
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
				info.size = sizes[index];
				Buffers[index] = gpu::CreateBuffer(Device, &info);
				REQUIRE(Buffers[index]);
			}
			for (auto &texture : Textures) {
				SDL_GPUTextureCreateInfo info{};
				info.type = SDL_GPU_TEXTURETYPE_2D;
				info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
				info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
							 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
				info.width = info.height = EXTENT;
				info.layer_count_or_depth = info.num_levels = 1;
				info.sample_count = SDL_GPU_SAMPLECOUNT_1;
				texture = gpu::CreateTexture(Device, &info);
				REQUIRE(texture);
			}
			SDL_GPUSamplerCreateInfo sampler{};
			Sampler = SDL_CreateGPUSampler(Device, &sampler);
			REQUIRE(Sampler);
		}

		Image ReadImage() const {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = ROW_BYTES * EXTENT;
			const auto release = [this](SDL_GPUTransferBuffer *buffer) {
				gpu::ReleaseTransferBuffer(Device, buffer);
			};
			std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
				gpu::CreateTransferBuffer(Device, &info), release
			);
			REQUIRE(transfer);
			auto *command = SDL_AcquireGPUCommandBuffer(Device);
			REQUIRE(command);
			auto *copy = SDL_BeginGPUCopyPass(command);
			REQUIRE(copy);
			SDL_GPUTextureRegion source{};
			source.texture = Textures[1];
			source.w = source.h = EXTENT;
			source.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = transfer.get();
			destination.pixels_per_row = ROW_BYTES / 8;
			destination.rows_per_layer = EXTENT;
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			REQUIRE(fence);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!SDL_QueryGPUFence(Device, fence) && std::chrono::steady_clock::now() < deadline)
				SDL_Delay(1);
			const bool complete = SDL_QueryGPUFence(Device, fence);
			SDL_ReleaseGPUFence(Device, fence);
			REQUIRE(complete);
			const auto *mapped =
				static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(Device, transfer.get(), false));
			REQUIRE(mapped);
			Image image(EXTENT * EXTENT);
			for (uint32_t y = 0; y < EXTENT; ++y)
				for (uint32_t x = 0; x < EXTENT; ++x) {
					const size_t offset = y * (ROW_BYTES / 4) + x * 2;
					image[y * EXTENT + x] = glm::vec4(
						glm::unpackHalf2x16(mapped[offset]), glm::unpackHalf2x16(mapped[offset + 1])
					);
				}
			SDL_UnmapGPUTransferBuffer(Device, transfer.get());
			return image;
		}
	};

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

	std::vector<glm::ivec4> Pages(uint32_t lod, bool compact, bool evicted = false) {
		const int side = 8 << lod;
		std::vector<glm::ivec4> pages;
		uint32_t words = 0;
		for (int x = -16; x < 16; x += side)
			for (int y = 0; y < 16; y += side)
				for (int z = -16; z < 16; z += side) {
					if (evicted && x == 0) continue;
					const uint32_t kind = compact && x < 0 ? 1 : 0;
					pages.emplace_back(x, y, z, int(lod | (kind << 5) | (words << 7)));
					words += kind == 1 ? 512 : 1024;
				}
		return pages;
	}

	std::vector<glm::ivec4> Topology(std::span<const glm::ivec4> descriptors) {
		std::vector<glm::ivec4> topology(descriptors.begin(), descriptors.end());
		for (auto &key : topology)
			key.w &= 31;
		return topology;
	}

	Image RenderClouds(
		CloudDevice &gpu,
		WeatherRayUniforms rays,
		std::span<const glm::ivec4> pages,
		std::span<const glm::ivec4> previous = {},
		float transition = 1,
		bool denseReference = false
	) {
		auto *command = SDL_AcquireGPUCommandBuffer(gpu.Device);
		REQUIRE(command);
		Clear(command, gpu.Textures[0], {1, 0, 0, 1});
		REQUIRE(RecordWeatherPages(gpu.Device, command, gpu.Buffers[2], pages));
		const std::array dummy{glm::ivec4(0)};
		REQUIRE(RecordWeatherPages(
			gpu.Device,
			command,
			gpu.Buffers[3],
			previous.empty() ? std::span<const glm::ivec4>(dummy) : previous
		));
		if (!denseReference) {
			rays.Field.Grid = {8, 8, 8, uint32_t(pages.size()) * 512};
			rays.Field.Origin.w = 0;
			rays.Field.Bricks = {0, 0, 0, uint32_t(pages.size())};
			rays.Field.Phase.w = uint32_t(pages.back().w) & 31;
		}
		rays.Quality.z = uint32_t(previous.size());
		rays.Quality.w = std::bit_cast<uint32_t>(transition);
		REQUIRE(RecordWeatherField(command, gpu.Pipelines[0], gpu.Buffers[0], gpu.Buffers[2], rays.Field));
		REQUIRE(RecordWeatherSummary(
			command, gpu.Pipelines[1], gpu.Buffers[0], gpu.Buffers[2], gpu.Buffers[1], rays.Field
		));
		REQUIRE(RecordWeatherRays(
			command,
			gpu.Pipelines[2],
			gpu.Buffers[0],
			gpu.Buffers[1],
			gpu.Buffers[2],
			gpu.Buffers[3],
			gpu.Textures[0],
			gpu.Textures[1],
			gpu.Textures[2],
			gpu.Sampler,
			EXTENT,
			EXTENT,
			rays
		));
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		return gpu.ReadImage();
	}

	void Compare(const Image &reference, const Image &actual, float opticalError, const char *phase) {
		REQUIRE(actual.size() == reference.size());
		float maximumError = 0;
		for (size_t pixel = 0; pixel < actual.size(); ++pixel)
			for (uint32_t lane = 0; lane < 4; ++lane) {
				REQUIRE(std::isfinite(actual[pixel][lane]));
				maximumError = std::max(maximumError, std::abs(actual[pixel][lane] - reference[pixel][lane]));
			}
		INFO(phase << " maximum RGBA error=" << maximumError << " optical budget=" << opticalError);
		CHECK(maximumError <= opticalError + HALF_ALLOWANCE);
	}
}

TEST_CASE(
	"weather volume preserves sharp clouds across packed page seams eviction and refill",
	"[render][gpu][weather][cloud-seams][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	CloudDevice gpu{static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)};
	gpu.Initialise();
	scene::WeatherCompute controls;
	controls.Seed = 38172;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 16;
	controls.ExtentMetres = {32, 16, 32};
	controls.Coverage = 1;
	controls.CondensateScale = .0003f;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.BaseTemperatureCelsius = -50;
	controls.TemperatureVariationCelsius = controls.PressureVariationHectopascals = 0;
	controls.TurbulenceMetresPerSecond = controls.ShearMetresPerSecond = controls.LiftMetresPerSecond = 0;
	controls.WindMetresPerSecond = {};
	controls.FrontContrast = controls.GrowthRate = 0;
	controls.FeatureCells = 4;
	controls.LodError = .002f;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 251);
	REQUIRE(time);
	WeatherGpuGrid referenceGrid;
	referenceGrid.Origin = {-16, 0, -16};
	referenceGrid.Dimensions = {33, 17, 33};
	referenceGrid.BrickDimensions = {5, 3, 5};
	referenceGrid.CellCount = 33 * 17 * 33;
	referenceGrid.BrickCount = 75;
	const auto dense = Pages(0, false);
	const auto compact = Pages(0, true);
	const auto coarse = Pages(1, true);
	const auto evicted = Pages(0, true, true);
	const auto compactTopology = Topology(compact);
	const auto coarseTopology = Topology(coarse);
	const auto evictedTopology = Topology(evicted);
	for (float cameraX : {-.125f, .125f}) {
		scene::Camera camera;
		camera.NearPlane = .1f;
		camera.FarPlane = 100;
		camera.FieldOfViewRadians = .55f;
		const auto frame = core::CFrame::LookAt({cameraX, 8, 24}, {cameraX, 8, 0});
		const auto matrices = scene::ResolveCamera(frame, camera, 1);
		WeatherRayUniforms rays{};
		rays.InverseViewProjection = glm::inverse(matrices.ViewProjection);
		rays.Eye = {cameraX, 8, 24, 0};
		rays.Minimum = {-16, 0, -16, 1};
		rays.Maximum = {16, 16, 16, 100};
		rays.ColourExtinction = {.75f, .5f, .25f, weather::ExtinctionPerMetre(controls.CondensateScale)};
		rays.Sun = {0, -1, 0, 0};
		rays.SunColour = {0, 0, 0, 0};
		rays.Ambient = {1, 1, 1, 0};
		rays.Quality = {64, std::bit_cast<uint32_t>(controls.LodError), 0, std::bit_cast<uint32_t>(1.0f)};
		rays.Field = WeatherUniformsOf(compiled->Definition, *time, referenceGrid);
		const auto reference = RenderClouds(gpu, rays, dense, {}, 1, true);
		size_t visible = 0;
		float minimumTransmittance = 1;
		float maximumTransmittance = 0;
		for (const auto &pixel : reference) {
			REQUIRE(std::isfinite(pixel.w));
			visible += pixel.w < .99f && pixel.x > .005f;
			minimumTransmittance = std::min(minimumTransmittance, pixel.w);
			maximumTransmittance = std::max(maximumTransmittance, pixel.w);
		}
		REQUIRE(visible > reference.size() / 4);
		REQUIRE(maximumTransmittance - minimumTransmittance > .02f);
		Compare(reference, RenderClouds(gpu, rays, dense), 0, "dense page seams");
		Compare(reference, RenderClouds(gpu, rays, compact), controls.LodError, "compact/dense seams");
		Compare(reference, RenderClouds(gpu, rays, coarse), controls.LodError, "coarse compact topology");
		for (float alpha : {0.0f, .5f, 1.0f})
			Compare(
				reference,
				RenderClouds(gpu, rays, evicted, compactTopology, alpha),
				controls.LodError,
				"evicted fine pages"
			);
		for (float alpha : {0.0f, .5f, 1.0f})
			Compare(
				reference,
				RenderClouds(gpu, rays, compact, evictedTopology, alpha),
				controls.LodError,
				"refilled fine pages"
			);
		Compare(
			reference,
			RenderClouds(gpu, rays, compact, coarseTopology, .5f),
			controls.LodError,
			"coarse to fine refill"
		);
	}
}
