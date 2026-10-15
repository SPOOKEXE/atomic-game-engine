#include "WeatherGpu.hpp"

#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherAssembly.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/msl/Translate.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.render.weathergpu")
TEST_DEPENDS("engine.weather.weather")

namespace {
	using namespace engine;
	using namespace engine::render;

	SDL_GPUComputePipeline *RuntimeWeatherOraclePipeline(
		SDL_GPUDevice *device, std::string_view source, std::string_view label, uint32_t uniformBuffers = 0
	) {
		const auto formats = SDL_GetGPUShaderFormats(device);
		if (!(formats & (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL)))
			SKIP("weather kernel oracles require SPIR-V or runtime-translated MSL");
		ShaderCompiler compiler;
		const auto compiled = compiler.Compile(source, ShaderStage::Compute, label);
		INFO(compiled.Error);
		REQUIRE_FALSE(compiled.Failed);
		SDL_GPUComputePipelineCreateInfo info{};
		std::string metalSource;
		if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
			info.code = reinterpret_cast<const Uint8 *>(compiled.SpirV.data());
			info.code_size = compiled.SpirV.size() * sizeof(uint32_t);
			info.entrypoint = "main";
			info.format = SDL_GPU_SHADERFORMAT_SPIRV;
		} else {
			auto translated = msl::Translate(compiled.SpirV);
			INFO(translated.Error);
			REQUIRE_FALSE(translated.Failed);
			metalSource = std::move(translated.Source);
			info.code = reinterpret_cast<const Uint8 *>(metalSource.data());
			info.code_size = metalSource.size();
			info.entrypoint = msl::ENTRY_POINT;
			info.format = SDL_GPU_SHADERFORMAT_MSL;
		}
		info.num_readwrite_storage_buffers = 1;
		info.num_uniform_buffers = uniformBuffers;
		info.threadcount_x = 64;
		info.threadcount_y = info.threadcount_z = 1;
		return SDL_CreateGPUComputePipeline(device, &info);
	}

	SDL_GPUComputePipeline *FieldPipeline(SDL_GPUDevice *device) {
		const auto binary = ShaderBinaryFor(device);
		std::ifstream file(resources::Shader("weather-generate.comp", binary.Form), std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		REQUIRE(!bytes.empty());
		SDL_GPUComputePipelineCreateInfo info{};
		info.code = reinterpret_cast<const Uint8 *>(bytes.data());
		info.code_size = bytes.size();
		info.entrypoint = binary.EntryPoint;
		info.format = binary.Format;
		info.num_readonly_storage_buffers = 2;
		info.num_readwrite_storage_buffers = 1;
		info.num_uniform_buffers = 1;
		info.threadcount_x = 64;
		info.threadcount_y = 1;
		info.threadcount_z = 1;
		return SDL_CreateGPUComputePipeline(device, &info);
	}

	std::vector<std::array<uint32_t, 2>>
	Download(SDL_GPUDevice *device, SDL_GPUBuffer *cells, uint32_t count) {
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = count * 8;
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
		SDL_GPUBufferRegion region{cells, 0, info.size};
		SDL_GPUTransferBufferLocation location{transfer.get(), 0};
		SDL_DownloadFromGPUBuffer(copy, &region, &location);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
		);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped);
		std::vector<std::array<uint32_t, 2>> result(count);
		std::memcpy(result.data(), mapped, info.size);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return result;
	}

	// These graph resources must remain live through the fixture's final output node.
	test::CapturedImage CaptureHalfResource(
		Renderer &renderer, core::Name resource, size_t slot, uint32_t width, uint32_t height
	) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(resource, slot));
		REQUIRE(device);
		REQUIRE(texture);
		REQUIRE(width > 0);
		REQUIRE(height > 0);
		const uint32_t rowBytes = (width * 8 + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = rowBytes * height;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *buffer) {
			gpu::ReleaseTransferBuffer(device, buffer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			gpu::CreateTransferBuffer(device, &info), releaseTransfer
		);
		REQUIRE(transfer);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = rowBytes / 8;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
		);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		INFO("half-float capture timeout for " << resource.Text());
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped);
		test::CapturedImage image;
		image.Width = width;
		image.Height = height;
		image.Format = test::ImageFormat::Rgba32Float;
		image.RowStrideBytes = width * sizeof(float) * 4;
		image.Bytes.resize(image.RowStrideBytes * height);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const size_t offset = y * (rowBytes / 4) + x * 2;
				const auto redGreen = glm::unpackHalf2x16(mapped[offset]);
				const auto blueAlpha = glm::unpackHalf2x16(mapped[offset + 1]);
				const std::array<float, 4> pixel{redGreen.x, redGreen.y, blueAlpha.x, blueAlpha.y};
				std::memcpy(
					image.Bytes.data() + y * image.RowStrideBytes + x * sizeof(pixel),
					pixel.data(),
					sizeof(pixel)
				);
			}
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return image;
	}
}

TEST_CASE("weather GPU layout bounds the whole authored field", "[render][weather]") {
	scene::WeatherCompute controls;
	const auto grid = WeatherGridOf(controls);
	REQUIRE(grid);
	CHECK(grid->CellCount <= WEATHER_MAX_CELLS);
	CHECK(grid->BrickCount <= WEATHER_MAX_BRICKS);
	CHECK(grid->Dimensions.x * grid->Stride * controls.CellSizeMetres >= controls.ExtentMetres.X);
	CHECK(grid->Dimensions.z * grid->Stride * controls.CellSizeMetres >= controls.ExtentMetres.Z);
	controls.MaximumResidentBricks = 1;
	const auto coarse = WeatherGridOf(controls);
	REQUIRE(coarse);
	CHECK(coarse->BrickCount == 1);
	CHECK(coarse->Stride > grid->Stride);
	controls.CellSizeMetres = 0;
	CHECK_FALSE(WeatherGridOf(controls));
}

TEST_CASE("weather field keys keep integral ticks and explicit canonical controls", "[render][weather]") {
	scene::WeatherCompute controls;
	const auto compiled = weather::Compile(controls);
	const auto grid = WeatherGridOf(controls);
	REQUIRE(compiled);
	REQUIRE(grid);
	const auto time =
		weather::ResolveTime(weather::InitialAnchor(compiled->Definition), (uint64_t{1} << 60) + 7);
	REQUIRE(time);
	const auto uniforms = WeatherUniformsOf(compiled->Definition, *time, *grid);
	CHECK(uniforms.Phase.x == uint32_t(time->PhaseTick));
	CHECK(uniforms.Phase.y == uint32_t(time->PhaseTick >> 32));
	CHECK(
		WeatherSignature(uniforms) == WeatherSignature(WeatherUniformsOf(compiled->Definition, *time, *grid))
	);
	auto changed = uniforms;
	++changed.Phase.x;
	CHECK(WeatherSignature(uniforms) != WeatherSignature(changed));
	changed = uniforms;
	++changed.Identity.x;
	CHECK(WeatherSignature(uniforms) != WeatherSignature(changed));
	auto fractionalTime = *time;
	fractionalTime.PhaseFractionQ16 = 12345;
	CHECK(
		WeatherSignature(WeatherUniformsOf(compiled->Definition, fractionalTime, *grid)) ==
		WeatherSignature(uniforms)
	);
}

TEST_CASE(
	"stateful GPU geometry follows physical controls while timeline pauses or scales", "[render][weather]"
) {
	scene::WeatherCompute controls;
	controls.Mode = scene::WeatherMode::Stateful;
	controls.CellSizeMetres = 4;
	controls.ExtentMetres = {32, 32, 32};
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 32;
	const auto normal = WeatherSimulationGridOf(controls);
	REQUIRE(normal);
	controls.Paused = true;
	controls.TimeScale = .5f;
	const auto paused = WeatherSimulationGridOf(controls);
	REQUIRE(paused);
	CHECK(paused->Dimensions == normal->Dimensions);
	CHECK(paused->CellCount == normal->CellCount);
	CHECK(paused->Origin == normal->Origin);
}

TEST_CASE("stateful physical cache identity separates providers and model recipes", "[render][weather]") {
	scene::Environment environment;
	environment.Weather.Mode = scene::WeatherMode::Stateful;
	const auto originalLayer = environment.Weather.LayerName;
	const uint64_t original = WeatherPhysicalSignature(environment);
	environment.Weather.Paused = true;
	environment.Weather.TimeScale = .5f;
	CHECK(WeatherPhysicalSignature(environment) == original);
	environment.Weather.DefinitionRevision++;
	environment.Weather.WindMetresPerSecond = {17, -2, 3};
	environment.Weather.IceFraction = .75f;
	environment.Weather.FogStrength = .5f;
	environment.Weather.ExtentMetres.Y += 64;
	environment.CloudLayer.Density *= .5f;
	CHECK(WeatherPhysicalSignature(environment) == original);
	for (unsigned change = 0; change < 4; ++change) {
		auto physicalEdit = environment;
		if (change == 0) physicalEdit.Weather.TerrainRevision++;
		if (change == 1) physicalEdit.Weather.GroundVapourFluxKilogramsPerSquareMetreSecond = .001f;
		if (change == 2) physicalEdit.Weather.RainFallSpeedMetresPerSecond += 1;
		if (change == 3) physicalEdit.Weather.BoundaryX = scene::WeatherBoundary::Open;
		CHECK(WeatherPhysicalSignature(physicalEdit) != original);
	}
	environment.Weather.Seed++;
	CHECK(WeatherPhysicalSignature(environment) != original);
	environment.Weather.Seed--;
	environment.Weather.LayerName = core::Name("another-layer");
	CHECK(WeatherPhysicalSignature(environment) != original);
	environment.Weather.LayerName = originalLayer;
	environment.CloudProvider.Id++;
	CHECK(WeatherPhysicalSignature(environment) != original);
}

TEST_CASE("cloud GPU lighting carries scene night and coloured sunlight", "[render][weather]") {
	scene::WorldLighting lighting;
	lighting.Direction = {0, -2, 0};
	lighting.Ambient = {.125f, .25f, .375f};
	lighting.OutdoorAmbient = {.25f, .5f, .75f};
	lighting.Direct = {1, .25f, 0};
	const auto day = WeatherLightingOf(lighting);
	CHECK(day.Direction == glm::vec4(0, -1, 0, 0));
	CHECK(day.Ambient == glm::vec4(.25f, .5f, .75f, 0));
	CHECK(day.Direct == glm::vec4(1, .25f, 0, 0));
	lighting.Ambient = {};
	lighting.OutdoorAmbient = {};
	lighting.Direct = {};
	lighting.Direction = {};
	const auto night = WeatherLightingOf(lighting);
	CHECK(night.Direct == glm::vec4(0));
	CHECK(night.Ambient == glm::vec4(0));
	CHECK(night.Direction == glm::vec4(0, -1, 0, 0));
}

TEST_CASE(
	"sparse GPU dense pages and compact assembly words match canonical references",
	"[render][gpu][weather][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		FieldPipeline(device), releasePipeline
	);
	REQUIRE(pipeline);
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 64;
	controls.ExtentMetres = {64, 64, 64};
	controls.Coverage = .9f;
	controls.VapourMixingRatio = .025f;
	const auto compiled = weather::Compile(controls);
	const auto grid = WeatherGridOf(controls);
	REQUIRE(compiled);
	REQUIRE(grid);
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	SDL_GPUBufferCreateInfo info{};
	info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	info.size = 4096 + WEATHER_COMPRESSED_PAGE_BYTES;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> cells(
		gpu::CreateBuffer(device, &info), releaseBuffer
	);
	info.size = 32;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> pages(
		gpu::CreateBuffer(device, &info), releaseBuffer
	);
	REQUIRE(cells);
	REQUIRE(pages);
	const std::array<glm::ivec4, 2> descriptors{{{-8, 8, -8, 0}, {-16, 16, -16, 2 | 32 | (1024 << 7)}}};
	for (uint64_t tick : {uint64_t{0}, uint64_t{251}, (uint64_t{1} << 60) + 29}) {
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		auto uniforms = WeatherUniformsOf(compiled->Definition, *time, *grid);
		uniforms.Grid = {8, 8, 8, 1024};
		uniforms.Origin.w = 0;
		uniforms.Bricks = {0, 0, 0, 2};
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		REQUIRE(RecordWeatherPages(device, command, pages.get(), descriptors));
		REQUIRE(RecordWeatherField(command, pipeline.get(), cells.get(), pages.get(), uniforms, 6144));
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		const auto actual = Download(device, cells.get(), 768);
		for (uint32_t index = 0; index < 512; ++index) {
			const auto expected = weather::Evaluate(
				compiled->Definition,
				*time,
				{-8 + int32_t(index % 8), 8 + int32_t((index / 8) % 8), -8 + int32_t(index / 64)}
			);
			for (uint32_t lane = 0; lane < 8; ++lane)
				REQUIRE(((actual[index][lane / 4] >> (8 * (lane % 4))) & 255u) == expected.Codes[lane]);
		}
		for (uint32_t group = 0; group < 64; ++group) {
			std::array<weather::EncodedCell, 8> children{};
			for (uint32_t child = 0; child < 8; ++child)
				children[child] = weather::Evaluate(
					compiled->Definition,
					*time,
					{-16 + int32_t((group % 4) * 2 + (child & 1)) * 4,
					 16 + int32_t(((group / 4) % 4) * 2 + ((child >> 1) & 1)) * 4,
					 -16 + int32_t((group / 16) * 2 + ((child >> 2) & 1)) * 4}
				);
			const auto expected = PackWeatherAssembly(children);
			REQUIRE(expected);
			WeatherPackedAssembly packed;
			for (uint32_t word = 0; word < 8; ++word)
				packed.Words[word] = actual[512 + group * 4 + word / 2][word % 2];
			packed.Words[1] &= ~(1u << 23);
			CHECK(packed == *expected);
			REQUIRE(UnpackWeatherAssembly(packed));
		}
	}
}

TEST_CASE("weather GPU generated words agree with canonical CPU samples", "[render][gpu][weather][.]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		FieldPipeline(device), releasePipeline
	);
	REQUIRE(pipeline);
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 1;
	controls.ExtentMetres = {17, 17, 17};
	controls.BaseHeightMetres = -8;
	controls.ThicknessMetres = 17;
	controls.WindMetresPerSecond = {-12, 2, -7};
	controls.Coverage = .9f;
	controls.VapourMixingRatio = .025f;
	const auto compiled = weather::Compile(controls);
	const auto grid = WeatherGridOf(controls);
	REQUIRE(compiled);
	REQUIRE(grid);
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	bufferInfo.size = grid->CellCount * 8;
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> cells(
		gpu::CreateBuffer(device, &bufferInfo), releaseBuffer
	);
	REQUIRE(cells);
	SDL_GPUBufferCreateInfo pageInfo{};
	pageInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	pageInfo.size = 16;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> pages(
		gpu::CreateBuffer(device, &pageInfo), releaseBuffer
	);
	REQUIRE(pages);
	struct SampleCase {
		uint64_t Tick;
		uint32_t FeatureShift;
		weather::CellCoordinate Origin;
	};
	const uint32_t shift = compiled->Definition.FeatureShift;
	const auto origin = grid->Origin;
	const weather::CellCoordinate lowHigh{INT32_MIN, origin.Y, INT32_MAX - 16};
	const weather::CellCoordinate highLow{INT32_MAX - 16, origin.Y, INT32_MIN};
	for (const auto sample :
		 {SampleCase{0, shift, origin},
		  SampleCase{6, shift, origin},
		  SampleCase{7, shift, origin},
		  SampleCase{128, shift, origin},
		  SampleCase{249, shift, origin},
		  SampleCase{250, shift, origin},
		  SampleCase{251, shift, origin},
		  SampleCase{(uint64_t{1} << 60) + 29, shift, origin},
		  SampleCase{255, shift, origin},
		  SampleCase{256, shift, origin},
		  SampleCase{(uint64_t{1} << 40) - 1, shift, origin},
		  SampleCase{uint64_t{1} << 40, shift, origin},
		  SampleCase{(uint64_t{1} << 40) + 1, shift, origin},
		  SampleCase{std::numeric_limits<uint64_t>::max(), shift, origin},
		  SampleCase{(uint64_t{1} << 40) + 6, 0, lowHigh},
		  SampleCase{(uint64_t{1} << 40) + 7, 0, lowHigh},
		  SampleCase{(uint64_t{1} << 40) + 249, 10, highLow},
		  SampleCase{(uint64_t{1} << 40) + 250, 10, highLow}}) {
		const uint64_t tick = sample.Tick;
		auto definition = compiled->Definition;
		definition.FeatureShift = sample.FeatureShift;
		auto sampleGrid = *grid;
		sampleGrid.Origin = sample.Origin;
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		REQUIRE(RecordWeatherField(
			command,
			pipeline.get(),
			cells.get(),
			pages.get(),
			WeatherUniformsOf(definition, *time, sampleGrid)
		));
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		const auto actual = Download(device, cells.get(), grid->CellCount);
		for (uint32_t index = 0; index < grid->CellCount; ++index) {
			const weather::CellCoordinate coordinate{
				sampleGrid.Origin.X + int32_t(index % grid->Dimensions.x) * int32_t(grid->Stride),
				sampleGrid.Origin.Y +
					int32_t((index / grid->Dimensions.x) % grid->Dimensions.y) * int32_t(grid->Stride),
				sampleGrid.Origin.Z +
					int32_t(index / (grid->Dimensions.x * grid->Dimensions.y)) * int32_t(grid->Stride)
			};
			const auto expected = weather::Evaluate(definition, *time, coordinate);
			for (uint32_t lane = 0; lane < 8; ++lane) {
				INFO(
					"tick=" << tick << " shift=" << sample.FeatureShift << " index=" << index
							<< " lane=" << lane
				);
				REQUIRE(((actual[index][lane / 4] >> (8 * (lane % 4))) & 255u) == expected.Codes[lane]);
			}
		}
	}
}

TEST_CASE(
	"weather GPU world rays clip outside the layer and retain same-tick cells", "[render][gpu][weather][.]"
) {
	using namespace engine;
	using namespace engine::render;
	const bool captureDiagnostics = GENERATE(true, false);
	INFO("pinned half-float diagnostics=" << captureDiagnostics);
	test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name pipeline(captureDiagnostics ? "weather.gpu.diagnostic" : "weather.gpu.capture");
	const core::Name boundary(captureDiagnostics ? "weather-test-diagnostic" : "weather-test-output");
	graph::PipelineDocument document = graph::DefaultPbrDocument();
	graph::NodeKindSpec spec;
	spec.Kind = boundary;
	spec.Scope = graph::NodeScope::Frame;
	spec.Queue = graph::ExecutionQueue::Cpu;
	spec.Category = graph::NodeCategory::Output;
	std::vector<core::Name> capturedResources{core::Name("tonemapped")};
	if (captureDiagnostics)
		for (const auto resource : {"sky-lit", "weather-lit", "weather-clouds", "weather-cloud-depth"})
			capturedResources.emplace_back(resource);
	for (const auto resource : capturedResources)
		spec.Inputs.push_back({.Name = resource, .Kind = graph::ResourceKind::Texture});
	REQUIRE(graph::RegisterNodeKind(std::move(spec)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundary, [](const graph::RunContext &) { return true; }));
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = boundary,
		 .NodeKind = boundary,
		 .Scope = graph::NodeScope::Frame}
	);
	for (const auto resource : capturedResources)
		document.Record({.Kind = graph::EditKind::Reads, .Target = resource, .Key = resource});
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(pipeline, graph));
	constexpr uint32_t EXTENT = 33;
	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.World = 773;
	view.WorldName = core::Name("weather.gpu.world");
	view.Pipeline = pipeline;
	view.Target = &target;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {.1f, .1f, .1f};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {1, 1, 1};
	view.Lighting.FogStart = 1000;
	view.Lighting.FogEnd = 1001;
	auto &environment = view.Lighting.EnvironmentState;
	environment.HasClouds = true;
	environment.HasWeatherCompute = true;
	environment.CloudLayer.Colour = {1, .1f, .05f};
	environment.CloudLayer.Density = 1;
	environment.Weather.CellSizeMetres = 1;
	environment.Weather.ExtentMetres = {16, 16, 16};
	environment.Weather.BaseHeightMetres = -8;
	environment.Weather.ThicknessMetres = 16;
	environment.Weather.Coverage = 1;
	environment.Weather.VapourMixingRatio = .029f;
	environment.Weather.BaseTemperatureCelsius = -50;
	environment.Weather.FeatureCells = 8;
	environment.Weather.WindMetresPerSecond = {};
	environment.Weather.Seed = 17;
	view.Camera.FarPlane = 32;
	view.CameraFrame.Position = {};
	OverlayImage overlay;
	const auto insideFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(insideFrame.Submitted);
	CHECK(insideFrame.Ran(core::Name("weather-integrate")));
	const auto inside = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	view.CameraFrame.Position = {0, 0, -20};
	view.Camera.FarPlane = 8;
	const auto clippedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(clippedFrame.Submitted);
	const auto clipped = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(clipped.View(), inside.View()).MismatchedPixels > 0);
	view.CameraFrame.Position = {};
	view.Camera.FarPlane = 32;
	view.Lighting.Ambient = {};
	view.Lighting.Direct = {};
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto night = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(inside.View(), night.View()).MismatchedPixels > 0);
	view.Lighting.Direct = {0, 1, 0};
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto greenSun = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(night.View(), greenSun.View()).MismatchedPixels > 0);
	CHECK(test::CompareImages(inside.View(), greenSun.View()).MismatchedPixels > 0);
	view.Lighting.Ambient = {.1f, .1f, .1f};
	view.Lighting.Direct = {1, 1, 1};
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? value->Value : 0.0;
	};
	const double generatedBeforeWarm = counter("render.weather.field_dispatches");
	const auto warmFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(warmFrame.Submitted);
	CHECK(warmFrame.ComputeDispatches <= insideFrame.ComputeDispatches);
	CHECK(counter("render.weather.field_dispatches") == generatedBeforeWarm);
	environment.HasWeatherClock = true;
	environment.Clock.Tick = 1;
	environment.Clock.WindQ20 = {};
	const auto changedTickFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(changedTickFrame.Submitted);
	CHECK(counter("render.weather.field_dispatches") == generatedBeforeWarm + 1);
	environment.Clock.Tick = 0;
	environment.Weather.Enabled = false;
	const auto disabledFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(disabledFrame.Submitted);
	const auto disabled = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(clipped.View(), disabled.View()).Passed());
	environment.Weather.Enabled = true;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto restored = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(inside.View(), restored.View()).Passed());

	// Both images start with empty renderer-owned weather history.
	const double beforeDeviceRecreation = counter("render.weather.field_dispatches");
	fixture.Render.Shutdown();
	REQUIRE(fixture.Render.Initialise(nullptr));
	REQUIRE(fixture.Render.IsHeadless());
	REQUIRE(fixture.Render.SetPipeline(pipeline, graph));
	const auto recreatedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(recreatedFrame.Submitted);
	CHECK(recreatedFrame.Ran(core::Name("weather-integrate")));
	CHECK(counter("render.weather.field_dispatches") == beforeDeviceRecreation + 1);
	const auto recreated = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(inside.View(), recreated.View()).Passed());

	environment.Weather.ShadowsEnabled = false;
	view.CameraFrame.Position = {0, 0, 12};
	scene::DrawInstance occluder;
	occluder.Frame.Position = {0, 0, 10};
	occluder.HalfExtent = {20, 20, .1f};
	occluder.EmissiveTint = {.2f, .4f, .7f};
	occluder.EmissiveStrength = 1;
	view.Instances = std::span(&occluder, 1);
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto opaqueWeather = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	if (captureDiagnostics) {
		const auto halfExtent = fixture.Render.ResourceTextureExtent(core::Name("weather-clouds"), view.Slot);
		const auto rawClouds = CaptureHalfResource(
			fixture.Render,
			core::Name("weather-clouds"),
			view.Slot,
			halfExtent.DrawnWidth,
			halfExtent.DrawnHeight
		);
		const auto cloudDepth = CaptureHalfResource(
			fixture.Render,
			core::Name("weather-cloud-depth"),
			view.Slot,
			halfExtent.DrawnWidth,
			halfExtent.DrawnHeight
		);
		const auto opaqueSky =
			CaptureHalfResource(fixture.Render, core::Name("sky-lit"), view.Slot, EXTENT, EXTENT);
		const auto opaqueWeatherLit =
			CaptureHalfResource(fixture.Render, core::Name("weather-lit"), view.Slot, EXTENT, EXTENT);
		auto neutralClouds = rawClouds;
		const std::array<float, 4> neutralPixel{0, 0, 0, 1};
		for (size_t offset = 0; offset < neutralClouds.Bytes.size(); offset += sizeof(neutralPixel))
			std::memcpy(neutralClouds.Bytes.data() + offset, neutralPixel.data(), sizeof(neutralPixel));
		test::CheckImage(
			fixture.Render,
			"weather-opaque-wall",
			"raw-clouds",
			"opaque wall fully occludes cloud bounds",
			neutralClouds.View(),
			rawClouds.View()
		);
		REQUIRE(test::CompareImages(neutralClouds.View(), rawClouds.View()).Passed());
		for (size_t offset = 3 * sizeof(float); offset < cloudDepth.Bytes.size();
			 offset += 4 * sizeof(float)) {
			float hit = -1;
			std::memcpy(&hit, cloudDepth.Bytes.data() + offset, sizeof(hit));
			REQUIRE(hit == 0);
		}
		test::CheckImage(
			fixture.Render,
			"weather-opaque-wall",
			"neutral-reconstruction",
			"expected: sky-lit FP16; actual: weather-lit FP16 with all raw clouds neutral",
			opaqueSky.View(),
			opaqueWeatherLit.View()
		);
	}
	environment.Weather.Enabled = false;
	const double beforeDisabled = counter("render.weather.field_dispatches");
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto opaqueClear = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	test::CheckImage(
		fixture.Render,
		"weather-opaque-wall",
		"tonemapped",
		"33x33 camera=(0,0,12), emissive wall z=10 halfExtent=(20,20,.1), weather "
		"bounds=(-8,-8,-8)..(8,8,8); "
		"expected: weather disabled; actual: weather enabled after inside-cloud history and device "
		"recreation",
		opaqueClear.View(),
		opaqueWeather.View()
	);
	CHECK(counter("render.weather.field_dispatches") == beforeDisabled);
	CHECK_FALSE(fixture.Render.HasPendingWeatherWork(view.World));
	view.Instances = {};
	view.CameraFrame.Position = {};
	environment.Weather.Coverage = 0;
	environment.Weather.Enabled = true;
	const double beforeEmpty = counter("render.weather.field_dispatches");
	const double beforeEmptyCopies = counter("render.weather.empty_binding_initialized_bytes");
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	CHECK(counter("render.weather.field_dispatches") == beforeEmpty);
	CHECK(counter("render.weather.empty_binding_initialized_bytes") == beforeEmptyCopies + 8);
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	CHECK(counter("render.weather.field_dispatches") == beforeEmpty);
	CHECK(counter("render.weather.empty_binding_initialized_bytes") == beforeEmptyCopies + 8);
}

TEST_CASE("stateful GPU shares one physical owner across two graph pipelines", "[render][gpu][weather][.]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(graph::DefaultPbrDocument(), graph, offender) == graph::PipelineDocumentStatus::Ok);
	const std::array<core::Name, 2> pipelines{
		core::Name("weather.stateful.first"), core::Name("weather.stateful.second")
	};
	for (auto pipeline : pipelines)
		REQUIRE(fixture.Render.SetPipeline(pipeline, graph));
	scene::WeatherCompute controls;
	controls.Mode = scene::WeatherMode::Stateful;
	controls.CellSizeMetres = 64;
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 256;
	controls.ExtentMetres = {256, 256, 256};
	controls.TickRate = 20;
	controls.TurbulenceMetresPerSecond = 0;
	controls.ShearMetresPerSecond = 0;
	controls.LiftMetresPerSecond = 0;
	controls.PressureVariationHectopascals = 0;
	controls.TemperatureVariationCelsius = 0;
	controls.WindMetresPerSecond = {4, 0, 0};
	controls.ShadowsEnabled = false;
	const auto definition = weather::MakeSimulationDefinition(controls, "weather");
	REQUIRE(definition);
	weather::SolverCell initial;
	initial.VapourKilogramsPerKilogram = .012f;
	initial.CondensateKilogramsPerCubicMetre = .004f;
	initial.TemperatureCelsius = 15;
	initial.AtmosphericPressurePascals = 101325;
	initial.VelocityMetresPerSecond = {4, 0, 0};
	weather::SimulationForcing forcing;
	forcing.WindTargetMetresPerSecond = {4, 0, 0};
	forcing.WindRelaxationPerSecond = 1;
	forcing.Inflow = initial;
	const auto simulation = weather::CreateSimulation(*definition, initial, forcing);
	REQUIRE(simulation);
	const auto checkpoint = weather::EncodeCheckpoint(*simulation);
	REQUIRE(checkpoint);
	std::array<SceneTarget, 2> targets{{{16, 16}, {16, 16}}};
	std::array<View, 2> views;
	for (size_t index = 0; index < views.size(); ++index) {
		auto &view = views[index];
		view.World = 974;
		view.WorldName = core::Name("weather.shared.physical.world");
		view.Pipeline = pipelines[index];
		view.Target = &targets[index];
		view.Slot = index;
		view.OverrideLighting = true;
		view.Lighting.EnvironmentState.HasClouds = true;
		view.Lighting.EnvironmentState.HasWeatherCompute = true;
		view.Lighting.EnvironmentState.HasWeatherClock = true;
		view.Lighting.EnvironmentState.Weather = controls;
		view.Lighting.EnvironmentState.Clock.Epoch = definition->WorldEpoch;
		view.CameraFrame.Position = {0, 128, 0};
		view.WeatherCheckpoint = *checkpoint;
		view.WeatherSimulationLayer = core::Name("weather");
		view.WeatherCheckpointVersion = 1;
		view.WeatherCheckpointEpoch = definition->WorldEpoch;
		view.WeatherCheckpointRevision = definition->Revision;
	}
	const auto counter = [](std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? value->Value : 0.0;
	};
	const double beforeQueues = counter("render.weather.stateful.queue_submissions");
	const double beforeAllocations = counter("render.weather.stateful.allocated_bytes");
	OverlayImage overlay;
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	const auto settle = [&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (fixture.Render.HasPendingWeatherWork(views[0].World) &&
			   std::chrono::steady_clock::now() < deadline) {
			SDL_Delay(1);
			REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
		}
		REQUIRE_FALSE(fixture.Render.HasPendingWeatherWork(views[0].World));
	};
	settle();
	CHECK(counter("render.weather.stateful.queue_submissions") == beforeQueues + 1);
	CHECK(counter("render.weather.stateful.allocated_bytes") == beforeAllocations + 260 * 64 + 32);
	const auto firstPhysicalOwners = core::Metrics::GetGauge("render.weather.stateful.physical_owners");
	REQUIRE(firstPhysicalOwners);
	CHECK(firstPhysicalOwners->Value == 1);
	for (auto &view : views)
		view.Lighting.EnvironmentState.Clock.Tick = 1;
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	settle();
	CHECK(counter("render.weather.stateful.queue_submissions") == beforeQueues + 2);
	CHECK(counter("render.weather.stateful.allocated_bytes") == beforeAllocations + 260 * 64 + 32);
	for (auto &view : views) {
		view.Lighting.EnvironmentState.Clock.Paused = true;
		view.Lighting.EnvironmentState.Clock.AnchorTick = 1;
		view.Lighting.EnvironmentState.Clock.PhaseTick = 1;
		view.Lighting.EnvironmentState.Weather.Paused = true;
		view.Lighting.EnvironmentState.Weather.TimeScale = .5f;
	}
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	CHECK(counter("render.weather.stateful.queue_submissions") == beforeQueues + 2);
	const double residentAllocations = counter("render.weather.stateful.allocated_bytes");
	const double residentCopies = counter("render.weather.stateful.packed_copy_bytes");
	for (auto &view : views) {
		auto &environment = view.Lighting.EnvironmentState;
		environment.Weather.DefinitionRevision++;
		environment.Weather.WindMetresPerSecond = {8, 0, 0};
		environment.Weather.IceFraction = .75f;
		environment.Weather.FogStrength = .5f;
		environment.CloudLayer.Colour = {.25f, .5f, .75f};
		environment.CloudLayer.Density *= .5f;
	}
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	const auto sharedPhysicalOwners = core::Metrics::GetGauge("render.weather.stateful.physical_owners");
	REQUIRE(sharedPhysicalOwners);
	CHECK(sharedPhysicalOwners->Value == 1);
	CHECK(counter("render.weather.stateful.allocated_bytes") == residentAllocations);
	CHECK(counter("render.weather.stateful.queue_submissions") == beforeQueues + 2);
	CHECK(counter("render.weather.stateful.packed_copy_bytes") == residentCopies);
	auto updated = *simulation;
	REQUIRE(weather::StepSimulation(updated) == weather::SimulationStatus::Success);
	updated.Definition.Revision = views[0].Lighting.EnvironmentState.Weather.DefinitionRevision;
	forcing.Tick = updated.Tick;
	forcing.WindTargetMetresPerSecond = {8, 0, 0};
	REQUIRE(weather::AddSimulationForcing(updated, forcing));
	const auto forcingCheckpoint = weather::EncodeCheckpoint(updated);
	REQUIRE(forcingCheckpoint);
	for (auto &view : views) {
		view.WeatherCheckpoint = *forcingCheckpoint;
		view.WeatherCheckpointVersion++;
		view.WeatherCheckpointRevision = updated.Definition.Revision;
		view.WeatherCheckpointTick = updated.Tick;
		view.Lighting.EnvironmentState.Clock.Paused = false;
		view.Lighting.EnvironmentState.Clock.Tick = 2;
		view.Lighting.EnvironmentState.Weather.Paused = false;
		view.Lighting.EnvironmentState.Weather.TimeScale = 1;
	}
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	settle();
	CHECK(counter("render.weather.stateful.queue_submissions") == beforeQueues + 3);
	const auto resumedPhysicalOwners = core::Metrics::GetGauge("render.weather.stateful.physical_owners");
	REQUIRE(resumedPhysicalOwners);
	CHECK(resumedPhysicalOwners->Value == 1);
	CHECK(counter("render.weather.stateful.allocated_bytes") == residentAllocations);
	for (auto &view : views)
		view.Lighting.EnvironmentState.Weather.Enabled = false;
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	const auto releasedPhysicalOwners = core::Metrics::GetGauge("render.weather.stateful.physical_owners");
	REQUIRE(releasedPhysicalOwners);
	CHECK(releasedPhysicalOwners->Value == 0);
	CHECK_FALSE(fixture.Render.HasPendingWeatherWork(views[0].World));
}

TEST_CASE(
	"weather GPU compact density decode agrees for every legal sum child and unsafe marker",
	"[render][gpu][weather][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
							"resources/shaders/weather-assembly.glsl";
	REQUIRE(sourcePath.is_absolute());
	std::ifstream includeFile(sourcePath, std::ios::binary);
	REQUIRE(includeFile.good());
	const std::string assembly{std::istreambuf_iterator<char>(includeFile), std::istreambuf_iterator<char>()};
	REQUIRE_FALSE(assembly.empty());
	const std::string source = "#version 450\n" + assembly + R"(
layout(local_size_x = 64) in;
layout(set = 1, binding = 0, std430) writeonly buffer Results { uvec2 values[]; } outputBuffer;
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= 2041u * 8u * 2u) return;
	uint sum = index / 16u;
	uint child = (index / 2u) & 7u;
	uint unsafe = index & 1u;
	uint remaining = sum;
	uvec2 children[8];
	for (uint i = 0u; i < 8u; ++i) {
		uint density = min(remaining, 255u);
		remaining -= density;
		uint varied = index * 0x9e3779b9u + i * 0x85ebca6bu;
		uint temperature = varied % 255u;
		uint vapour = (varied >> 7u) & 255u;
		uint pressure = (varied >> 13u) % 255u;
		uint windX = (varied >> 3u) % 255u;
		uint windY = (varied >> 11u) % 255u;
		uint windZ = (varied >> 19u) % 255u;
		uint ice = (varied >> 24u) & 255u;
		children[i] = uvec2(density | (temperature << 8u) | (vapour << 16u) | (pressure << 24u),
			windX | (windY << 8u) | (windZ << 16u) | (ice << 24u));
	}
	uint words[8];
	WeatherAssemblyPack(children, words);
	words[1] = (words[1] & ~(1u << 23u)) | (unsafe << 23u);
	uvec2 scalarWords = uvec2(words[0], words[1]);
	uint specialized = WeatherAssemblyDensityCode(scalarWords, child) |
		(uint(WeatherAssemblyUnsafe(scalarWords)) << 8u);
	uint generic = (WeatherAssemblyDecode(words, child).x & 255u) |
		(uint(WeatherAssemblyUnsafe(words)) << 8u);
	outputBuffer.values[index] = uvec2(specialized, generic);
}
)";
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		RuntimeWeatherOraclePipeline(device, source, "weather-assembly-oracle.comp"), releasePipeline
	);
	INFO(SDL_GetError());
	REQUIRE(pipeline);
	constexpr uint32_t CASES = 2041 * 8 * 2;
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	bufferInfo.size = CASES * sizeof(std::array<uint32_t, 2>);
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> output(
		gpu::CreateBuffer(device, &bufferInfo), releaseBuffer
	);
	REQUIRE(output);
	const auto cancelCommand = [](SDL_GPUCommandBuffer *command) {
		(void)SDL_CancelGPUCommandBuffer(command);
	};
	std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
		SDL_AcquireGPUCommandBuffer(device), cancelCommand
	);
	REQUIRE(command);
	SDL_GPUStorageBufferReadWriteBinding binding{};
	binding.buffer = output.get();
	auto *pass = SDL_BeginGPUComputePass(command.get(), nullptr, 0, &binding, 1);
	REQUIRE(pass);
	SDL_BindGPUComputePipeline(pass, pipeline.get());
	SDL_DispatchGPUCompute(pass, (CASES + 63) / 64, 1, 1);
	SDL_EndGPUComputePass(pass);
	REQUIRE(SDL_SubmitGPUCommandBuffer(command.release()));
	const auto actual = Download(device, output.get(), CASES);
	for (uint32_t index = 0; index < CASES; ++index) {
		const uint32_t sum = index / 16;
		const uint32_t child = (index / 2) & 7;
		const uint32_t unsafe = index & 1;
		const uint32_t expected = (sum / 8 + uint32_t(child < sum % 8)) | (unsafe << 8);
		CAPTURE(sum, child, unsafe);
		REQUIRE(actual[index][0] == expected);
		REQUIRE(actual[index][1] == expected);
	}
}

TEST_CASE(
	"weather GPU density shortcut preserves canonical boundary and phase samples", "[render][gpu][weather][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
							"resources/shaders/weather-common.glsl";
	REQUIRE(sourcePath.is_absolute());
	std::ifstream includeFile(sourcePath, std::ios::binary);
	REQUIRE(includeFile.good());
	const std::string common{std::istreambuf_iterator<char>(includeFile), std::istreambuf_iterator<char>()};
	REQUIRE_FALSE(common.empty());
	const std::string source = "#version 450\n" + common + R"(
layout(local_size_x = 64) in;
layout(set = 1, binding = 0, std430) writeonly buffer Results { uvec2 values[]; } outputBuffer;
layout(set = 2, binding = 0) uniform Pass { WeatherField Field; } pass;
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= 74u) return;
	const ivec2 horizontal[8] = ivec2[8](ivec2(0), ivec2(-1, 1),
		ivec2(int(0x80000000u), 0x7fffffff), ivec2(0x7fffffff, int(0x80000000u)),
		ivec2(16777215, -16777216), ivec2(-16777216, 16777215),
		ivec2(-33, 17), ivec2(29, -47));
	ivec3 coordinate;
	if (index < 72u) {
		ivec2 xz = horizontal[index / 9u];
		coordinate = ivec3(xz.x, int(pass.Field.Shape.x) + int(index % 9u) - 1, xz.y);
	} else {
		coordinate = index == 72u ? ivec3(int(0x80000000u)) : ivec3(0x7fffffff);
	}
	outputBuffer.values[index] = uvec2(WeatherDensityCode(pass.Field, coordinate),
		WeatherEvaluate(pass.Field, coordinate).x);
}
)";
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		RuntimeWeatherOraclePipeline(device, source, "weather-density-oracle.comp", 1), releasePipeline
	);
	INFO(SDL_GetError());
	REQUIRE(pipeline);
	constexpr uint32_t CASES = 74;
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
	bufferInfo.size = CASES * sizeof(std::array<uint32_t, 2>);
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> output(
		gpu::CreateBuffer(device, &bufferInfo), releaseBuffer
	);
	REQUIRE(output);
	scene::WeatherCompute controls;
	const auto compiled = weather::Compile(controls);
	const auto grid = WeatherGridOf(controls);
	REQUIRE(compiled);
	REQUIRE(grid);
	const std::array<std::array<int32_t, 2>, 8> horizontal{
		{{0, 0},
		 {-1, 1},
		 {INT32_MIN, INT32_MAX},
		 {INT32_MAX, INT32_MIN},
		 {16777215, -16777216},
		 {-16777216, 16777215},
		 {-33, 17},
		 {29, -47}}
	};
	for (const uint32_t shift : {0u, 10u})
		for (const uint64_t tick : {uint64_t{0}, uint64_t{128}, uint64_t{250}, uint64_t{256}, UINT64_MAX}) {
			auto definition = compiled->Definition;
			definition.BaseCell = -3;
			definition.ThicknessCells = 7;
			definition.FeatureShift = shift;
			definition.Coverage = 255;
			definition.Vapour = 255;
			definition.SaturationBase = 0;
			definition.SaturationLapseQ8 = 0;
			definition.TemperatureSaturationQ8 = 0;
			const auto time = weather::ResolveTime(weather::InitialAnchor(definition), tick);
			REQUIRE(time);
			const auto uniforms = WeatherUniformsOf(definition, *time, *grid);
			const auto cancelCommand = [](SDL_GPUCommandBuffer *command) {
				(void)SDL_CancelGPUCommandBuffer(command);
			};
			std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
				SDL_AcquireGPUCommandBuffer(device), cancelCommand
			);
			REQUIRE(command);
			SDL_GPUStorageBufferReadWriteBinding binding{};
			binding.buffer = output.get();
			auto *pass = SDL_BeginGPUComputePass(command.get(), nullptr, 0, &binding, 1);
			REQUIRE(pass);
			SDL_BindGPUComputePipeline(pass, pipeline.get());
			SDL_PushGPUComputeUniformData(command.get(), 0, &uniforms, sizeof(uniforms));
			SDL_DispatchGPUCompute(pass, (CASES + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
			REQUIRE(SDL_SubmitGPUCommandBuffer(command.release()));
			const auto actual = Download(device, output.get(), CASES);
			bool hasPositiveDensity = false;
			bool hasZeroEnvelope = false;
			for (uint32_t index = 0; index < CASES; ++index) {
				const weather::CellCoordinate coordinate =
					index < 72
						? weather::
							  CellCoordinate{horizontal[index / 9][0], -4 + int32_t(index % 9), horizontal[index / 9][1]}
					: index == 72 ? weather::CellCoordinate{INT32_MIN, INT32_MIN, INT32_MIN}
								  : weather::CellCoordinate{INT32_MAX, INT32_MAX, INT32_MAX};
				const auto expected = weather::Evaluate(definition, *time, coordinate);
				hasPositiveDensity |= expected.Codes[0] > 0;
				const int64_t altitude = int64_t(coordinate.Y) - definition.BaseCell;
				hasZeroEnvelope |= altitude < 0 || uint64_t(altitude) >= definition.ThicknessCells;
				uint32_t expectedWord = 0;
				for (uint32_t lane = 0; lane < 4; ++lane)
					expectedWord |= uint32_t(expected.Codes[lane]) << (lane * 8);
				CAPTURE(shift, tick, index);
				REQUIRE(actual[index][0] == uint32_t(expected.Codes[0]));
				REQUIRE(actual[index][1] == expectedWord);
			}
			CAPTURE(shift, tick);
			REQUIRE(hasPositiveDensity);
			REQUIRE(hasZeroEnvelope);
		}
}
