#include "WeatherGpu.hpp"

#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherAssembly.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.render.weathergpu")
TEST_DEPENDS("engine.weather.weather")

namespace {
	using namespace engine;
	using namespace engine::render;

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
		info.num_readonly_storage_buffers = 1;
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
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence));
		SDL_ReleaseGPUFence(device, fence);
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped);
		std::vector<std::array<uint32_t, 2>> result(count);
		std::memcpy(result.data(), mapped, info.size);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return result;
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
	for (const uint64_t tick :
		 {uint64_t{0},
		  uint64_t{251},
		  (uint64_t{1} << 60) + 29,
		  uint64_t{255},
		  uint64_t{256},
		  (uint64_t{1} << 40) - 1,
		  uint64_t{1} << 40,
		  (uint64_t{1} << 40) + 1,
		  std::numeric_limits<uint64_t>::max()}) {
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		REQUIRE(RecordWeatherField(
			command,
			pipeline.get(),
			cells.get(),
			pages.get(),
			WeatherUniformsOf(compiled->Definition, *time, *grid)
		));
		REQUIRE(SDL_SubmitGPUCommandBuffer(command));
		const auto actual = Download(device, cells.get(), grid->CellCount);
		for (uint32_t index = 0; index < grid->CellCount; ++index) {
			const weather::CellCoordinate coordinate{
				grid->Origin.X + int32_t(index % grid->Dimensions.x) * int32_t(grid->Stride),
				grid->Origin.Y +
					int32_t((index / grid->Dimensions.x) % grid->Dimensions.y) * int32_t(grid->Stride),
				grid->Origin.Z +
					int32_t(index / (grid->Dimensions.x * grid->Dimensions.y)) * int32_t(grid->Stride)
			};
			const auto expected = weather::Evaluate(compiled->Definition, *time, coordinate);
			for (uint32_t lane = 0; lane < 8; ++lane) {
				INFO("tick=" << tick << " index=" << index << " lane=" << lane);
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
	test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name pipeline("weather.gpu.capture");
	const core::Name boundary("weather-test-output");
	graph::PipelineDocument document = graph::DefaultPbrDocument();
	graph::NodeKindSpec spec;
	spec.Kind = boundary;
	spec.Scope = graph::NodeScope::Frame;
	spec.Queue = graph::ExecutionQueue::Cpu;
	spec.Category = graph::NodeCategory::Output;
	spec.Inputs.push_back({.Name = core::Name("tonemapped"), .Kind = graph::ResourceKind::Texture});
	REQUIRE(graph::RegisterNodeKind(std::move(spec)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundary, [](const graph::RunContext &) { return true; }));
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = boundary,
		 .NodeKind = boundary,
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("tonemapped"), .Key = core::Name("tonemapped")}
	);
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
	environment.Weather.Enabled = false;
	const double beforeDisabled = counter("render.weather.field_dispatches");
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	const auto opaqueClear = test::CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
	);
	CHECK(test::CompareImages(opaqueWeather.View(), opaqueClear.View()).Passed());
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
	CHECK(counter("render.weather.stateful.physical_owners") == 1);
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
	CHECK(counter("render.weather.stateful.physical_owners") == 1);
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
	CHECK(counter("render.weather.stateful.physical_owners") == 1);
	CHECK(counter("render.weather.stateful.allocated_bytes") == residentAllocations);
	for (auto &view : views)
		view.Lighting.EnvironmentState.Weather.Enabled = false;
	REQUIRE(fixture.Render.Render(views, overlay, nullptr, false).Submitted);
	CHECK(counter("render.weather.stateful.physical_owners") == 0);
	CHECK_FALSE(fixture.Render.HasPendingWeatherWork(views[0].World));
}
