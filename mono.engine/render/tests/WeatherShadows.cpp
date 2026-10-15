#include "WeatherShadows.hpp"

#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherGpu.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <glm/packing.hpp>

#include <cmath>
#include <fstream>
#include <limits>

TEST_SUITE_ID("engine.render.weathershadows")
TEST_DEPENDS("engine.render.weathergpu")

namespace {
	using namespace engine;
	using namespace engine::render;

	WeatherGpuInputs Inputs(const scene::WeatherCompute &controls, uint64_t tick = 0) {
		const auto compiled = weather::Compile(controls);
		const auto grid = WeatherGridOf(controls);
		REQUIRE(compiled);
		REQUIRE(grid);
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		return {WeatherUniformsOf(compiled->Definition, *time, *grid), *compiled, *time, false};
	}

	WeatherResidencyView ViewAt(core::Vector3 position) {
		WeatherResidencyView view;
		view.View.CameraPositionMetres = position;
		return view;
	}

	SDL_GPUComputePipeline *WeatherPipeline(SDL_GPUDevice *device, bool generateField = false) {
		const auto binary = ShaderBinaryFor(device);
		std::ifstream file(
			resources::Shader(generateField ? "weather-generate.comp" : "weather-shadow.comp", binary.Form),
			std::ios::binary
		);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		REQUIRE_FALSE(bytes.empty());
		SDL_GPUComputePipelineCreateInfo description{};
		description.code = reinterpret_cast<const Uint8 *>(bytes.data());
		description.code_size = bytes.size();
		description.entrypoint = binary.EntryPoint;
		description.format = binary.Format;
		description.num_readonly_storage_buffers = generateField ? 1 : 2;
		description.num_readwrite_storage_buffers = generateField ? 1 : 0;
		description.num_readwrite_storage_textures = generateField ? 0 : 1;
		description.num_uniform_buffers = 1;
		description.threadcount_x = generateField ? 64 : 8;
		description.threadcount_y = generateField ? 1 : 8;
		description.threadcount_z = 1;
		return SDL_CreateGPUComputePipeline(device, &description);
	}

	std::array<float, 4> ReadCentre(Renderer &renderer, uint32_t extent) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture =
			static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(core::Name("lighting-baseline"), 0));
		REQUIRE(texture);
		SDL_GPUTransferBufferCreateInfo description{};
		description.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		description.size = 256;
		const auto release = [device](SDL_GPUTransferBuffer *transfer) {
			gpu::ReleaseTransferBuffer(device, transfer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
			gpu::CreateTransferBuffer(device, &description), release
		);
		REQUIRE(transfer);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureRegion from{};
		from.texture = texture;
		from.x = from.y = extent / 2;
		from.w = from.h = from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = transfer.get();
		to.pixels_per_row = 16;
		to.rows_per_layer = 1;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence));
		SDL_ReleaseGPUFence(device, fence);
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped);
		std::array<float, 4> result;
		std::memcpy(result.data(), mapped, sizeof(result));
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return result;
	}
}

TEST_CASE("weather shadows cover offscreen casters and union receivers", "[render][weather][shadows]") {
	scene::WeatherCompute controls;
	controls.ShadowCascades = 4;
	const auto inputs = Inputs(controls);
	const std::array views{ViewAt({-1000, 5, 0}), ViewAt({1000, 20, 0})};
	const auto plan = BuildWeatherShadowPlan(controls, inputs, {-.8f, -.6f, 0}, 1, 1, views);
	REQUIRE(plan.Enabled);
	CHECK(plan.Cascades == 4);
	CHECK(plan.Resolution == 256);
	CHECK(plan.Lighting.ToSun.w >= controls.ShadowDistanceMetres + 1000);
	CHECK(plan.Casters.Maximum.X > plan.Casters.Minimum.X);
	CHECK(plan.Casters.Minimum.Y < controls.BaseHeightMetres);
	CHECK(plan.Casters.Maximum.Y > controls.BaseHeightMetres + controls.ThicknessMetres);
	CHECK(plan.Casters.Maximum.X > controls.ShadowDistanceMetres);
	for (uint32_t index = 1; index < plan.Cascades; ++index)
		CHECK(plan.Lighting.Regions[index].z == 2 * plan.Lighting.Regions[index - 1].z);
	std::array<WeatherResidencyView, 3> demands{views[0], views[1], {}};
	CHECK(AppendWeatherShadowDemand(demands, 2, plan, inputs.Compiled.Definition.LayerKey) == 3);
	CHECK(demands.back().View.CameraPositionMetres == plan.Casters.Centre());
	CHECK(demands.back().MaximumDistanceMetres >= plan.Casters.Size().Y * .5f);
}

TEST_CASE(
	"weather shadow updates obey cadence and immediate semantic changes", "[render][weather][shadows]"
) {
	scene::WeatherCompute controls;
	const std::array views{ViewAt({0, 5, 0})};
	const auto first = BuildWeatherShadowPlan(controls, Inputs(controls, 0), {0, -1, 0}, 1, 1, views);
	const auto warm = BuildWeatherShadowPlan(controls, Inputs(controls, 3), {0, -1, 0}, 1, 1, views);
	const auto due = BuildWeatherShadowPlan(controls, Inputs(controls, 4), {0, -1, 0}, 1, 1, views);
	REQUIRE(first.Enabled);
	CHECK(first.Signature == warm.Signature);
	CHECK(first.Signature != due.Signature);
	CHECK(
		first.Signature !=
		BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, .5f, 1, views).Signature
	);
	CHECK_FALSE(BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, 0, 1, views).Enabled);
	const auto sunChanged = BuildWeatherShadowPlan(controls, Inputs(controls), {-.1f, -1, 0}, 1, 1, views);
	CHECK(first.Signature != sunChanged.Signature);
	CHECK(
		first.Signature !=
		BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, 1, 2, views).Signature
	);
	controls.Coverage = .8f;
	CHECK(
		first.Signature !=
		BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, 1, 1, views).Signature
	);
	controls.ShadowsEnabled = false;
	CHECK_FALSE(BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, 1, 1, views).Enabled);
	controls.ShadowsEnabled = true;
	CHECK_FALSE(BuildWeatherShadowPlan(controls, Inputs(controls), {1, 0, 0}, 1, 1, views).Enabled);
	CHECK_FALSE(BuildWeatherShadowPlan(controls, Inputs(controls), {1, .5f, 0}, 1, 1, views).Enabled);
	CHECK_FALSE(BuildWeatherShadowPlan(controls, Inputs(controls), {}, 1, 1, views).Enabled);
}

TEST_CASE(
	"weather shadow map keeps subtexel cameras stable and caps sunrise footprint",
	"[render][weather][shadows]"
) {
	scene::WeatherCompute controls;
	const auto inputs = Inputs(controls);
	const std::array firstViews{ViewAt({1, 5, 1})};
	const std::array nearbyViews{ViewAt({2, 5, 2})};
	const auto first = BuildWeatherShadowPlan(controls, inputs, {0, -1, 0}, 1, 1, firstViews);
	const auto nearby = BuildWeatherShadowPlan(controls, inputs, {0, -1, 0}, 1, 1, nearbyViews);
	CHECK(first.Signature == nearby.Signature);
	const std::array teleportedViews{ViewAt({500, 5, 0})};
	CHECK(
		first.Signature !=
		BuildWeatherShadowPlan(controls, inputs, {0, -1, 0}, 1, 1, teleportedViews).Signature
	);
	controls.ShadowDistanceMetres = 32;
	const auto sunrise = BuildWeatherShadowPlan(controls, inputs, {-.999f, -.03f, 0}, 1, 1, firstViews);
	CHECK_FALSE(sunrise.Enabled);
	controls.ShadowDistanceMetres = 8192;
	const auto low = BuildWeatherShadowPlan(controls, inputs, {-.998f, -.06f, 0}, 1, 1, firstViews);
	REQUIRE(low.Enabled);
	CHECK(low.Lighting.Receiver.w > 0);
	CHECK(low.Lighting.Receiver.w < 1);
}

TEST_CASE("weather shadows only publish maps from accepted commands", "[render][weather][shadows]") {
	scene::WeatherCompute controls;
	const std::array views{ViewAt({0, 0, 0})};
	const auto plan = BuildWeatherShadowPlan(controls, Inputs(controls), {0, -1, 0}, 1, 1, views);
	WeatherShadowCache cache;
	int firstCommand = 0;
	int otherCommand = 0;
	CHECK(cache.LightingFor(&firstCommand) == nullptr);
	cache.Stage(plan, nullptr, &firstCommand);
	CHECK(cache.LightingFor(&firstCommand) != nullptr);
	CHECK(cache.LightingFor(&otherCommand) == nullptr);
	cache.Commit(&otherCommand);
	CHECK(cache.LightingFor(&otherCommand) == nullptr);
	cache.Commit(&firstCommand);
	CHECK(cache.LightingFor(&otherCommand) != nullptr);
	cache.Stage(plan, nullptr, &firstCommand);
	cache.Discard(&firstCommand);
	CHECK(cache.LightingFor(&firstCommand) == nullptr);
}

TEST_CASE(
	"weather GPU shadow bands integrate only canonical resident water", "[render][gpu][weather][shadows][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		WeatherPipeline(device), releasePipeline
	);
	REQUIRE(pipeline);
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	SDL_GPUBufferCreateInfo bufferDescription{};
	bufferDescription.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	bufferDescription.size = 8 * 4 * 8 * 8;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> cells(
		gpu::CreateBuffer(device, &bufferDescription), releaseBuffer
	);
	bufferDescription.size = 16;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> pages(
		gpu::CreateBuffer(device, &bufferDescription), releaseBuffer
	);
	REQUIRE(cells);
	REQUIRE(pages);
	SDL_GPUTransferBufferCreateInfo uploadDescription{};
	uploadDescription.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	uploadDescription.size = 8 * 4 * 8 * 8;
	const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
		gpu::ReleaseTransferBuffer(device, transfer);
	};
	std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> upload(
		gpu::CreateTransferBuffer(device, &uploadDescription), releaseTransfer
	);
	REQUIRE(upload);
	auto *mapped = static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(device, upload.get(), false));
	REQUIRE(mapped);
	for (uint32_t cell = 0; cell < 8 * 4 * 8; ++cell) {
		mapped[cell * 2] = 255;
		mapped[cell * 2 + 1] = 0;
	}
	SDL_UnmapGPUTransferBuffer(device, upload.get());
	SDL_GPUTextureCreateInfo textureDescription{};
	textureDescription.type = SDL_GPU_TEXTURETYPE_2D;
	textureDescription.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
	textureDescription.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
	textureDescription.width = textureDescription.height = 32;
	textureDescription.layer_count_or_depth = textureDescription.num_levels = 1;
	const auto releaseTexture = [device](SDL_GPUTexture *texture) { gpu::ReleaseTexture(device, texture); };
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> map(
		gpu::CreateTexture(device, &textureDescription), releaseTexture
	);
	REQUIRE(map);
	auto *command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	auto *copy = SDL_BeginGPUCopyPass(command);
	REQUIRE(copy);
	const SDL_GPUTransferBufferLocation source{upload.get(), 0};
	const SDL_GPUBufferRegion destination{cells.get(), 0, uploadDescription.size};
	SDL_UploadToGPUBuffer(copy, &source, &destination, false);
	SDL_EndGPUCopyPass(copy);
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 1;
	controls.ThicknessMetres = 4;
	controls.ExtentMetres = {8, 4, 8};
	auto inputs = Inputs(controls);
	inputs.Stateful = true;
	inputs.Uniforms.Grid = {8, 4, 8, 256};
	inputs.Uniforms.Origin = {-4, 1, -4, 1};
	WeatherShadowPlan plan;
	plan.Enabled = true;
	plan.Resolution = 32;
	plan.Cascades = 1;
	plan.Lighting.Regions[0] = {0, 0, 4, 0};
	plan.Lighting.Controls = {1, 1, 5, 1};
	plan.Lighting.ToSun = {0, 1, 0, 8};
	plan.Lighting.Receiver = {0, 0, 0, 1};
	REQUIRE(RecordWeatherShadows(
		command, pipeline.get(), cells.get(), pages.get(), map.get(), plan, inputs, controls, 1
	));
	REQUIRE(SDL_SubmitGPUCommandBuffer(command));
	SDL_GPUTransferBufferCreateInfo downloadDescription{};
	downloadDescription.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	downloadDescription.size = 256;
	std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> download(
		gpu::CreateTransferBuffer(device, &downloadDescription), releaseTransfer
	);
	REQUIRE(download);
	command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	copy = SDL_BeginGPUCopyPass(command);
	const SDL_GPUTextureRegion from{map.get(), 0, 0, 16, 16, 0, 1, 1, 1};
	SDL_GPUTextureTransferInfo to{};
	to.transfer_buffer = download.get();
	to.pixels_per_row = 32;
	to.rows_per_layer = 1;
	SDL_DownloadFromGPUTexture(copy, &from, &to);
	SDL_EndGPUCopyPass(copy);
	auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
	REQUIRE(fence);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
		SDL_Delay(1);
	REQUIRE(SDL_QueryGPUFence(device, fence));
	SDL_ReleaseGPUFence(device, fence);
	const auto *half = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(device, download.get(), false));
	REQUIRE(half);
	const auto first = glm::unpackHalf2x16(half[0]);
	const auto second = glm::unpackHalf2x16(half[1]);
	SDL_UnmapGPUTransferBuffer(device, download.get());
	const float extinction = weather::ExtinctionPerMetre(controls.CondensateScale);
	CHECK(first.x == Catch::Approx(extinction).epsilon(.002));
	CHECK(first.y == Catch::Approx(extinction).epsilon(.002));
	CHECK(second.x == Catch::Approx(extinction).epsilon(.002));
	CHECK(second.y == Catch::Approx(extinction * .5f).epsilon(.002));
}

TEST_CASE(
	"weather GPU shadow coarse unsafe fallback respects the canonical optical error bound",
	"[render][gpu][weather][shadows][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> fieldPipeline(
		WeatherPipeline(device, true), releasePipeline
	);
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> shadowPipeline(
		WeatherPipeline(device), releasePipeline
	);
	REQUIRE(fieldPipeline);
	REQUIRE(shadowPipeline);
	constexpr uint32_t COMPACT_BYTES = 2048;
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	SDL_GPUBufferCreateInfo bufferDescription{};
	bufferDescription.usage =
		SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	bufferDescription.size = COMPACT_BYTES;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> cells(
		gpu::CreateBuffer(device, &bufferDescription), releaseBuffer
	);
	bufferDescription.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	bufferDescription.size = sizeof(glm::ivec4);
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> pages(
		gpu::CreateBuffer(device, &bufferDescription), releaseBuffer
	);
	REQUIRE(cells);
	REQUIRE(pages);
	SDL_GPUTextureCreateInfo textureDescription{};
	textureDescription.type = SDL_GPU_TEXTURETYPE_2D;
	textureDescription.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
	textureDescription.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
	textureDescription.width = textureDescription.height = 32;
	textureDescription.layer_count_or_depth = textureDescription.num_levels = 1;
	const auto releaseTexture = [device](SDL_GPUTexture *texture) { gpu::ReleaseTexture(device, texture); };
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> map(
		gpu::CreateTexture(device, &textureDescription), releaseTexture
	);
	REQUIRE(map);
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 1;
	controls.ThicknessMetres = 32;
	controls.ExtentMetres = {128, 64, 128};
	controls.Coverage = 1;
	controls.CondensateScale = .0003f;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.BaseTemperatureCelsius = -50;
	controls.TemperatureVariationCelsius = 0;
	controls.PressureVariationHectopascals = 32;
	controls.TurbulenceMetresPerSecond = 16;
	controls.WindMetresPerSecond = {};
	controls.FrontContrast = 0;
	controls.GrowthRate = 0;
	controls.FeatureCells = 1;
	controls.LodError = .001f;
	auto inputs = Inputs(controls);
	inputs.Uniforms.Grid = {8, 8, 8, 512};
	inputs.Uniforms.Origin = {0, 0, 0, 0};
	inputs.Uniforms.Bricks = {1, 1, 1, 1};
	inputs.Uniforms.Phase.w = 3;
	// An aged compact LOD3 page must regenerate its sharp groups through canonical fallback.
	const std::array keys{glm::ivec4(0, 0, 0, 3 | (1 << 5))};
	auto *command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	REQUIRE(RecordWeatherPages(device, command, pages.get(), keys));
	REQUIRE(RecordWeatherField(
		command, fieldPipeline.get(), cells.get(), pages.get(), inputs.Uniforms, COMPACT_BYTES
	));
	WeatherShadowPlan plan;
	plan.Enabled = true;
	plan.Resolution = 32;
	plan.Cascades = 1;
	plan.Lighting.Regions[0] = {16, 16, 1, 0};
	plan.Lighting.Controls = {1, 1, 33, 1};
	plan.Lighting.ToSun = {0, 1, 0, 8};
	plan.Lighting.Receiver = {16, 16, 0, 1};
	REQUIRE(RecordWeatherShadows(
		command, shadowPipeline.get(), cells.get(), pages.get(), map.get(), plan, inputs, controls, 1
	));
	REQUIRE(SDL_SubmitGPUCommandBuffer(command));
	SDL_GPUTransferBufferCreateInfo downloadDescription{};
	downloadDescription.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	downloadDescription.size = 256 + COMPACT_BYTES;
	const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
		gpu::ReleaseTransferBuffer(device, transfer);
	};
	std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> download(
		gpu::CreateTransferBuffer(device, &downloadDescription), releaseTransfer
	);
	REQUIRE(download);
	command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	auto *copy = SDL_BeginGPUCopyPass(command);
	REQUIRE(copy);
	const SDL_GPUTextureRegion from{map.get(), 0, 0, 16, 16, 0, 1, 1, 1};
	SDL_GPUTextureTransferInfo to{};
	to.transfer_buffer = download.get();
	to.pixels_per_row = 32;
	to.rows_per_layer = 1;
	SDL_DownloadFromGPUTexture(copy, &from, &to);
	const SDL_GPUBufferRegion fieldFrom{cells.get(), 0, COMPACT_BYTES};
	const SDL_GPUTransferBufferLocation fieldTo{download.get(), 256};
	SDL_DownloadFromGPUBuffer(copy, &fieldFrom, &fieldTo);
	SDL_EndGPUCopyPass(copy);
	auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
	REQUIRE(fence);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
		SDL_Delay(1);
	REQUIRE(SDL_QueryGPUFence(device, fence));
	SDL_ReleaseGPUFence(device, fence);
	const auto *mapped =
		static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(device, download.get(), false));
	REQUIRE(mapped);
	const auto first = glm::unpackHalf2x16(mapped[0]);
	const auto second = glm::unpackHalf2x16(mapped[1]);
	const std::array actual{first.x, first.y, second.x, second.y};
	const std::array halfCodes{mapped[0] & 65535u, mapped[0] >> 16u, mapped[1] & 65535u, mapped[1] >> 16u};
	std::array<uint32_t, COMPACT_BYTES / sizeof(uint32_t)> packed;
	std::memcpy(packed.data(), mapped + 256 / sizeof(uint32_t), COMPACT_BYTES);
	SDL_UnmapGPUTransferBuffer(device, download.get());
	// The probe's eight corners touch exactly these three groups throughout its full vertical ray.
	for (uint32_t groupY = 0; groupY < 3; ++groupY) {
		const uint32_t group = 1 + 4 * (groupY + 4);
		INFO("coarse fallback group " << group);
		REQUIRE((packed[group * 8 + 1] & (1u << 23)) != 0);
	}
	const auto reference = [&](int32_t stride) {
		std::array<float, 4> bands{};
		constexpr float PROBE = 16.0f + 1.0f / 32;
		constexpr float STEP = .5f;
		for (uint32_t index = 0; index < 64; ++index) {
			const float height = controls.BaseHeightMetres + (float(index) + .5f) * STEP;
			const std::array point{PROBE / stride, height / stride, PROBE / stride};
			const std::array cell{
				int32_t(std::floor(point[0])) * stride,
				int32_t(std::floor(point[1])) * stride,
				int32_t(std::floor(point[2])) * stride
			};
			const std::array weight{
				point[0] - std::floor(point[0]),
				point[1] - std::floor(point[1]),
				point[2] - std::floor(point[2])
			};
			float density = 0;
			for (int32_t z = 0; z < 2; ++z)
				for (int32_t y = 0; y < 2; ++y)
					for (int32_t x = 0; x < 2; ++x) {
						const auto encoded = weather::Evaluate(
							inputs.Compiled.Definition,
							inputs.Time,
							{cell[0] + x * stride, cell[1] + y * stride, cell[2] + z * stride}
						);
						density += float(encoded.Codes[0]) / 255 * (x ? weight[0] : 1 - weight[0]) *
								   (y ? weight[1] : 1 - weight[1]) * (z ? weight[2] : 1 - weight[2]);
					}
			const uint32_t band = uint32_t((height - controls.BaseHeightMetres) / 8);
			bands[band] += density * weather::ExtinctionPerMetre(controls.CondensateScale) * STEP;
		}
		return bands;
	};
	const auto fine = reference(1);
	const auto coarse = reference(8);
	const auto sum = [](const std::array<float, 4> &values) {
		return values[0] + values[1] + values[2] + values[3];
	};
	REQUIRE(std::abs(std::exp(-sum(coarse)) - std::exp(-sum(fine))) > controls.LodError * 8);
	for (size_t receiverBand = 0; receiverBand < 4; ++receiverBand) {
		float actualTau = 0;
		float referenceTau = 0;
		float rounding = 0;
		for (size_t band = receiverBand; band < 4; ++band) {
			actualTau += actual[band];
			referenceTau += fine[band];
			// Half an FP16 ULP bounds map encoding; exp(-tau) cannot amplify absolute tau error.
			const uint32_t exponent = (halfCodes[band] >> 10u) & 31u;
			rounding += std::ldexp(1.0f, int(std::max(exponent, 1u)) - 26);
		}
		rounding += 64 * std::numeric_limits<float>::epsilon() * std::max(referenceTau, 1.0f);
		INFO(
			"receiver band " << receiverBand << ": actual tau=" << actualTau << ", fine tau=" << referenceTau
		);
		CHECK(std::abs(std::exp(-actualTau) - std::exp(-referenceTau)) <= controls.LodError + rounding);
	}
}

TEST_CASE(
	"weather GPU shadows attenuate direct sunlight and preserve ambient and emissive",
	"[render][gpu][weather][shadows][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	graph::PipelineDocument document;
	document.Record(
		{.Kind = graph::EditKind::AddResource,
		 .Name = core::Name("lighting-baseline"),
		 .Resource = graph::ResourceKind::Colour,
		 .Format = graph::ResourceFormat::RGBA32F}
	);
	bool deferredReached = false;
	const auto nativeDocument = graph::DefaultPbrDocument();
	for (const auto &edit : nativeDocument.Edits()) {
		if (edit.Kind == graph::EditKind::AddNode) {
			if (deferredReached) break;
			deferredReached = edit.Name == core::Name("deferred-lighting");
		}
		document.Record(edit);
		if (deferredReached && edit.Kind == graph::EditKind::Writes && edit.Target == core::Name("lit"))
			document.Record(
				{.Kind = graph::EditKind::Writes,
				 .Target = core::Name("lighting-baseline"),
				 .Key = core::Name("lighting-baseline")}
			);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name pipeline("weather.shadow.direct-fixture");
	REQUIRE(fixture.Render.SetPipeline(pipeline, graph));
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("weather.shadow.receiver");
	REQUIRE(fixture.Render.AddMesh(mesh, plane));
	scene::DrawInstance receiver;
	receiver.Source = 1;
	receiver.Mesh = mesh;
	receiver.Frame.Position = {0, 0, -4};
	receiver.HalfExtent = {4, 4, .01f};
	receiver.Tint = {.8f, .5f, .3f};
	receiver.CastShadow = false;
	constexpr uint32_t EXTENT = 33;
	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.World = 992;
	view.WorldName = core::Name("weather.shadow.fixture-world");
	view.Pipeline = pipeline;
	view.Target = &target;
	view.Instances = std::span(&receiver, 1);
	view.OverrideLighting = true;
	view.Lighting.Direction = {0, -.7071068f, -.7071068f};
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {1, 1, 1};
	view.Lighting.FogStart = 1000;
	view.Lighting.FogEnd = 1001;
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasClouds = environment.HasWeatherCompute = true;
	environment.CloudLayer.Density = 1;
	environment.CloudLayer.Cover = 1;
	auto &controls = environment.Weather;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 8;
	controls.ThicknessMetres = 8;
	controls.ExtentMetres = {64, 32, 64};
	controls.Coverage = 1;
	controls.VapourMixingRatio = .029f;
	controls.BaseTemperatureCelsius = -50;
	controls.FeatureCells = 16;
	controls.WindMetresPerSecond = {};
	controls.ShadowStrength = 1;
	controls.ShadowDistanceMetres = 128;
	controls.ShadowResolution = 32;
	controls.ShadowsEnabled = false;
	OverlayImage overlay;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto direct = ReadCentre(fixture.Render, EXTENT);
	controls.ShadowsEnabled = true;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto shadowed = ReadCentre(fixture.Render, EXTENT);
	CHECK(shadowed[0] < direct[0]);
	CHECK(shadowed[1] < direct[1]);
	CHECK(shadowed[2] < direct[2]);
	environment.CloudLayer.Enabled = false;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto shadowsOnly = ReadCentre(fixture.Render, EXTENT);
	for (uint32_t channel = 0; channel < 4; ++channel)
		CHECK(shadowsOnly[channel] == Catch::Approx(shadowed[channel]).margin(.00001));
	environment.CloudLayer.Cover = 0;
	const auto dispatchesBeforeNoCover = core::Metrics::Get("render.weather.shadow_dispatches");
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto noCover = ReadCentre(fixture.Render, EXTENT);
	const auto dispatchesAfterNoCover = core::Metrics::Get("render.weather.shadow_dispatches");
	CHECK(
		(dispatchesAfterNoCover ? dispatchesAfterNoCover->Value : 0) ==
		(dispatchesBeforeNoCover ? dispatchesBeforeNoCover->Value : 0)
	);
	for (uint32_t channel = 0; channel < 4; ++channel)
		CHECK(noCover[channel] == Catch::Approx(direct[channel]).margin(.00001));
	environment.CloudLayer.Cover = 1;
	view.Lighting.Direct = {};
	view.Lighting.Ambient = {.3f, .2f, .1f};
	receiver.EmissiveTint = {.1f, .2f, .3f};
	receiver.EmissiveStrength = 1;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto ambientShadowed = ReadCentre(fixture.Render, EXTENT);
	controls.ShadowsEnabled = false;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto ambientClear = ReadCentre(fixture.Render, EXTENT);
	for (uint32_t channel = 0; channel < 4; ++channel)
		CHECK(ambientShadowed[channel] == Catch::Approx(ambientClear[channel]).margin(.00001));
	controls.ShadowsEnabled = true;
	controls.Coverage = 0;
	const auto dispatchesBeforeEmpty = core::Metrics::Get("render.weather.shadow_dispatches");
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto dispatchesAfterEmpty = core::Metrics::Get("render.weather.shadow_dispatches");
	CHECK(
		(dispatchesAfterEmpty ? dispatchesAfterEmpty->Value : 0) ==
		(dispatchesBeforeEmpty ? dispatchesBeforeEmpty->Value : 0)
	);
	// A graph extension may decline to produce field data while the scene stays renderable.
	const core::Name missingFieldKind("weather.shadow.fixture-missing-field");
	const auto *fieldSpec = graph::NodeCatalogue::Find(core::Name("weather-field"));
	REQUIRE(fieldSpec);
	auto missingField = *fieldSpec;
	missingField.Kind = missingFieldKind;
	missingField.BuiltInBackend = false;
	REQUIRE(graph::RegisterNodeKind(std::move(missingField)));
	REQUIRE(fixture.Render.InstallNodeHandler(missingFieldKind, [](const graph::RunContext &) {
		return true;
	}));
	graph::PipelineDocument unavailableDocument;
	for (auto edit : document.Edits()) {
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("weather-field"))
			edit.NodeKind = missingFieldKind;
		unavailableDocument.Record(std::move(edit));
	}
	graph::RenderGraph unavailableGraph;
	REQUIRE(
		graph::Build(unavailableDocument, unavailableGraph, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name unavailablePipeline("weather.shadow.fixture-unavailable-field");
	REQUIRE(fixture.Render.SetPipeline(unavailablePipeline, unavailableGraph));
	view.Pipeline = unavailablePipeline;
	view.World = 993;
	view.WorldName = core::Name("weather.shadow.fixture-refused-world");
	view.Lighting.Direct = {1, 1, 1};
	view.Lighting.Ambient = {};
	receiver.EmissiveStrength = 0;
	controls.Coverage = 1;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto unavailable = ReadCentre(fixture.Render, EXTENT);
	for (uint32_t channel = 0; channel < 4; ++channel)
		CHECK(unavailable[channel] == Catch::Approx(direct[channel]).margin(.00001));
}
