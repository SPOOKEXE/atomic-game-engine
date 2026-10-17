#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherGpu.hpp"
#include "WeatherGpuClock.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.render.weathergpuclockgpu")
TEST_DEPENDS("engine.weather.weather")
TEST_DEPENDS("engine.render.weathergpuclock")

namespace {
	using namespace engine;
	using namespace engine::render;

	void CancelCommand(SDL_GPUCommandBuffer *command) {
		(void)SDL_CancelGPUCommandBuffer(command);
	}
	using Command = std::unique_ptr<SDL_GPUCommandBuffer, decltype(&CancelCommand)>;

	double Counter(const char *name) {
		const auto value = core::Metrics::Get(name);
		return value ? value->Value : 0;
	}

	struct ClockDevice {
		test::FixtureDevice Fixture;
		SDL_GPUDevice *Device = nullptr;
		WeatherGpuClock Clock;
		std::array<SDL_GPUComputePipeline *, 3> Pipelines{};
		SDL_GPUTransferBuffer *Download = nullptr;
		std::vector<SDL_GPUBuffer *> Buffers;

		~ClockDevice() {
			ReleaseWeatherGpuClock(Device, Clock);
			for (auto *buffer : Buffers)
				gpu::ReleaseBuffer(Device, buffer);
			if (Download) gpu::ReleaseTransferBuffer(Device, Download);
			for (auto *pipeline : Pipelines)
				if (pipeline) SDL_ReleaseGPUComputePipeline(Device, pipeline);
		}

		SDL_GPUComputePipeline *Pipeline(const char *name, uint32_t reads, uint32_t threads) {
			const auto binary = ShaderBinaryFor(Device);
			std::ifstream file(resources::Shader(name, binary.Form), std::ios::binary);
			const std::vector<char> bytes{
				std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()
			};
			REQUIRE_FALSE(bytes.empty());
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = reinterpret_cast<const Uint8 *>(bytes.data());
			info.code_size = bytes.size();
			info.format = binary.Format;
			info.entrypoint = binary.EntryPoint;
			info.num_readonly_storage_buffers = reads;
			info.num_readwrite_storage_buffers = 1;
			info.num_uniform_buffers = 1;
			info.threadcount_x = threads;
			info.threadcount_y = info.threadcount_z = 1;
			return SDL_CreateGPUComputePipeline(Device, &info);
		}

		void Initialise() {
			Fixture.Initialise();
			Device = static_cast<SDL_GPUDevice *>(Fixture.Render.Backend().Device);
			Pipelines[0] = Pipeline("weather-clock.comp", 0, 1);
			Pipelines[1] = Pipeline("weather-generate.comp", 2, 64);
			Pipelines[2] = Pipeline("weather-summary.comp", 3, 64);
			for (auto *pipeline : Pipelines)
				REQUIRE(pipeline);
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = WEATHER_BRICK_EDGE * WEATHER_BRICK_EDGE * WEATHER_BRICK_EDGE * 8;
			Download = gpu::CreateTransferBuffer(Device, &info);
			REQUIRE(Download);
		}

		Command Acquire() {
			Command command(SDL_AcquireGPUCommandBuffer(Device), CancelCommand);
			REQUIRE(command);
			return command;
		}

		void Submit(Command command) {
			const auto *token = command.get();
			REQUIRE(SDL_SubmitGPUCommandBuffer(command.release()));
			CommitWeatherGpuClock(Clock, token);
		}

		SDL_GPUBuffer *Buffer(uint32_t size, SDL_GPUBufferUsageFlags usage) {
			SDL_GPUBufferCreateInfo info{};
			info.usage = usage;
			info.size = size;
			auto *buffer = gpu::CreateBuffer(Device, &info);
			REQUIRE(buffer);
			Buffers.push_back(buffer);
			return buffer;
		}

		void Upload(SDL_GPUBuffer *buffer, std::span<const uint32_t> words) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = static_cast<uint32_t>(words.size_bytes());
			const auto release = [this](SDL_GPUTransferBuffer *transfer) {
				gpu::ReleaseTransferBuffer(Device, transfer);
			};
			std::unique_ptr<SDL_GPUTransferBuffer, decltype(release)> transfer(
				gpu::CreateTransferBuffer(Device, &info), release
			);
			REQUIRE(transfer);
			void *mapped = SDL_MapGPUTransferBuffer(Device, transfer.get(), false);
			REQUIRE(mapped);
			std::memcpy(mapped, words.data(), words.size_bytes());
			SDL_UnmapGPUTransferBuffer(Device, transfer.get());
			auto command = Acquire();
			auto *copy = SDL_BeginGPUCopyPass(command.get());
			REQUIRE(copy);
			const SDL_GPUTransferBufferLocation source{transfer.get(), 0};
			const SDL_GPUBufferRegion target{buffer, 0, info.size};
			SDL_UploadToGPUBuffer(copy, &source, &target, false);
			SDL_EndGPUCopyPass(copy);
			Submit(std::move(command));
		}

		std::vector<uint32_t> Read(SDL_GPUBuffer *buffer, uint32_t words) {
			REQUIRE(words <= 1024);
			auto command = Acquire();
			auto *copy = SDL_BeginGPUCopyPass(command.get());
			REQUIRE(copy);
			const SDL_GPUBufferRegion source{buffer, 0, words * 4};
			const SDL_GPUTransferBufferLocation target{Download, 0};
			SDL_DownloadFromGPUBuffer(copy, &source, &target);
			SDL_EndGPUCopyPass(copy);
			const auto release = [this](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(Device, fence); };
			std::unique_ptr<SDL_GPUFence, decltype(release)> fence(
				SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), release
			);
			REQUIRE(fence);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!SDL_QueryGPUFence(Device, fence.get()) && std::chrono::steady_clock::now() < deadline)
				SDL_Delay(1);
			REQUIRE(SDL_QueryGPUFence(Device, fence.get()));
			const void *mapped = SDL_MapGPUTransferBuffer(Device, Download, false);
			REQUIRE(mapped);
			std::vector<uint32_t> result(words);
			std::memcpy(result.data(), mapped, words * 4);
			SDL_UnmapGPUTransferBuffer(Device, Download);
			return result;
		}

		void Check(const WeatherGpuClockSample &sample) {
			const auto actual = Read(Clock.Buffer, 8);
			const std::array<uint32_t, 8> expected{
				uint32_t(sample.Time.PhaseTick),
				uint32_t(sample.Time.PhaseTick >> 32),
				0,
				0,
				sample.Time.DisplacementQ8[0],
				sample.Time.DisplacementQ8[1],
				sample.Time.DisplacementQ8[2],
				sample.Time.Revision
			};
			REQUIRE(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
		}

		void Stage(const WeatherGpuClockSample &sample, bool duplicate = false) {
			auto command = Acquire();
			const auto plan = PlanWeatherGpuClock(Clock, sample, command.get());
			REQUIRE(plan);
			const auto dispatches = Counter("render.weather.clock_dispatches");
			const auto anchors = Counter("render.weather.clock_anchors");
			const auto advances = Counter("render.weather.clock_advances");
			const auto hits = Counter("render.weather.clock_cache_hits");
			const auto bytes = Counter("render.weather.clock_uniform_bytes");
			REQUIRE(RecordWeatherGpuClock(Device, command.get(), Pipelines[0], Clock, sample));
			if (duplicate) REQUIRE(RecordWeatherGpuClock(Device, command.get(), Pipelines[0], Clock, sample));
			const bool reuse = plan->Action == WeatherGpuClockAction::Reuse;
			CHECK(Counter("render.weather.clock_dispatches") == dispatches + (reuse ? 0 : 1));
			CHECK(
				Counter("render.weather.clock_anchors") ==
				anchors + uint32_t(plan->Action == WeatherGpuClockAction::Anchor)
			);
			CHECK(
				Counter("render.weather.clock_advances") ==
				advances + uint32_t(plan->Action == WeatherGpuClockAction::Advance)
			);
			CHECK(Counter("render.weather.clock_uniform_bytes") == bytes + (reuse ? 0 : 32));
			CHECK(Counter("render.weather.clock_cache_hits") == hits + uint32_t(reuse) + uint32_t(duplicate));
			Submit(std::move(command));
			Check(sample);
		}
	};

	void CompareCells(
		std::span<const uint32_t> actual,
		const weather::WeatherDefinition &definition,
		const weather::WeatherTime &time,
		const WeatherGpuGrid &grid
	) {
		for (uint32_t index = 0; index < grid.CellCount; ++index) {
			const weather::CellCoordinate coordinate{
				grid.Origin.X + int32_t(index % grid.Dimensions.x) * int32_t(grid.Stride),
				grid.Origin.Y +
					int32_t((index / grid.Dimensions.x) % grid.Dimensions.y) * int32_t(grid.Stride),
				grid.Origin.Z +
					int32_t(index / (grid.Dimensions.x * grid.Dimensions.y)) * int32_t(grid.Stride)
			};
			const auto expected = weather::Evaluate(definition, time, coordinate);
			for (uint32_t lane = 0; lane < 8; ++lane) {
				CAPTURE(index, lane);
				REQUIRE(((actual[index * 2 + lane / 4] >> (8 * (lane % 4))) & 255u) == expected.Codes[lane]);
			}
		}
	}
}

TEST_CASE(
	"weather GPU clock advances exact resident words and preserves bounded residency",
	"[render][gpu][weather][clock][.]"
) {
	ClockDevice gpu;
	gpu.Initialise();
	WeatherGpuClockSample sample;
	sample.SourceTick = sample.Time.Tick = 100;
	sample.TickRate = 20;
	sample.Identity = 17;
	sample.Time.PhaseTick = (uint64_t{0x12345678} << 32) | 0xfffffffeu;
	sample.Time.DisplacementQ8 = {UINT32_MAX - 7, 2, UINT32_MAX};
	sample.Time.Revision = 7;
	const auto before = gpu::MemoryStatistics(gpu.Device);
	gpu.Stage(sample, true);
	auto *const buffer = gpu.Clock.Buffer;
	const auto resident = gpu::MemoryStatistics(gpu.Device);
	REQUIRE(buffer);
	CHECK(resident.BufferBytes == before.BufferBytes + 32);
	CHECK(resident.BufferAllocations == before.BufferAllocations + 1);
	CHECK(resident.TransferBufferAllocations == before.TransferBufferAllocations);
	for (const uint64_t increment : {uint64_t{1}, uint64_t{2}, uint64_t{3}, (uint64_t{1} << 32) + 4}) {
		++sample.SourceTick;
		sample.Time.Tick = sample.SourceTick;
		sample.Time.PhaseTick += increment;
		sample.Time.DisplacementQ8[0] += 17;
		sample.Time.DisplacementQ8[1] -= 3;
		sample.Time.DisplacementQ8[2] += 0x80000000u;
		{
			auto plannedCommand = gpu.Acquire();
			const auto plan = PlanWeatherGpuClock(gpu.Clock, sample, plannedCommand.get());
			REQUIRE(plan);
			CHECK(
				plan->Action ==
				(increment <= 40 ? WeatherGpuClockAction::Advance : WeatherGpuClockAction::Anchor)
			);
		}
		gpu.Stage(sample, true);
		CHECK(gpu.Clock.Buffer == buffer);
	}
	// Repeated presentation of a paused sample performs no resident update.
	{
		auto pausedCommand = gpu.Acquire();
		const auto paused = PlanWeatherGpuClock(gpu.Clock, sample, pausedCommand.get());
		REQUIRE(paused);
		CHECK(paused->Action == WeatherGpuClockAction::Reuse);
	}
	gpu.Stage(sample, true);
	CHECK(gpu.Clock.CommittedSample.Time.PhaseTick == sample.Time.PhaseTick);
	sample.SourceTick += sample.TickRate * 5;
	sample.Time.Tick = sample.SourceTick;
	++sample.Time.PhaseTick;
	{
		auto periodicCommand = gpu.Acquire();
		const auto periodic = PlanWeatherGpuClock(gpu.Clock, sample, periodicCommand.get());
		REQUIRE(periodic);
		CHECK(periodic->Action == WeatherGpuClockAction::Anchor);
	}
	gpu.Stage(sample, true);
	CHECK(gpu.Clock.AnchorTick == sample.SourceTick);
	// A delayed render consumes the complete logical delta instead of one frame step.
	sample.SourceTick += 1'000'000;
	sample.Time.Tick = sample.SourceTick;
	sample.Time.PhaseTick += (uint64_t{1} << 45) + 19;
	sample.Time.DisplacementQ8 = {0, UINT32_MAX, 123456789};
	gpu.Stage(sample, true);
	const auto accepted = sample;
	++sample.SourceTick;
	sample.Time.Tick = sample.SourceTick;
	sample.Time.PhaseTick += 7;
	sample.Time.DisplacementQ8[2] += 99;
	auto cancelled = gpu.Acquire();
	const auto *const cancelledToken = cancelled.get();
	REQUIRE(RecordWeatherGpuClock(gpu.Device, cancelled.get(), gpu.Pipelines[0], gpu.Clock, sample));
	REQUIRE(SDL_CancelGPUCommandBuffer(cancelled.release()));
	DiscardWeatherGpuClock(gpu.Clock, cancelledToken);
	CHECK(gpu.Clock.CommittedSample.Time.PhaseTick == accepted.Time.PhaseTick);
	{
		auto retryCommand = gpu.Acquire();
		const auto retry = PlanWeatherGpuClock(gpu.Clock, sample, retryCommand.get());
		REQUIRE(retry);
		CHECK(retry->Action == WeatherGpuClockAction::Anchor);
	}
	gpu.Stage(sample, true);
	// Definition identity replaces the anchor while retaining the physical allocation.
	++sample.Identity;
	++sample.Time.Revision;
	sample.Time.PhaseTick = 0;
	sample.Time.DisplacementQ8 = {};
	gpu.Stage(sample, true);
	CHECK(gpu.Clock.Buffer == buffer);
	const auto after = gpu::MemoryStatistics(gpu.Device);
	CHECK(after.LiveBytes == resident.LiveBytes);
	CHECK(after.AllocatedBytes == resident.AllocatedBytes);
	CHECK(after.BufferAllocations == resident.BufferAllocations);
	CHECK(after.TransferBufferAllocations == resident.TransferBufferAllocations);
	ReleaseWeatherGpuClock(gpu.Device, gpu.Clock);
	CHECK(gpu.Clock.Buffer == nullptr);
	CHECK(gpu::MemoryStatistics(gpu.Device).BufferBytes == before.BufferBytes);
}

TEST_CASE(
	"weather GPU field consumes resident time instead of deliberately incorrect uniforms",
	"[render][gpu][weather][clock][.]"
) {
	ClockDevice gpu;
	gpu.Initialise();
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 2;
	controls.ExtentMetres = {16, 16, 16};
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 16;
	controls.FeatureCells = 8;
	controls.Coverage = 1;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.BaseTemperatureCelsius = -50;
	controls.TemperatureVariationCelsius = 0;
	const auto compiled = weather::Compile(controls);
	const auto grid = WeatherGridOf(controls);
	REQUIRE(compiled);
	REQUIRE(grid);
	REQUIRE(grid->CellCount == 512);
	WeatherGpuClockSample sample;
	sample.TickRate = 20;
	sample.Identity = 31;
	sample.SourceTick = sample.Time.Tick = 128;
	sample.Time.PhaseTick = (uint64_t{1} << 40) + 128;
	sample.Time.DisplacementQ8 = {UINT32_MAX - 123, 37, 0x80000021u};
	sample.Time.Revision = compiled->Definition.Revision;
	auto *cells = gpu.Buffer(4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
	auto *pages = gpu.Buffer(16, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
	weather::WeatherTime wrongTime;
	wrongTime.Revision = sample.Time.Revision;
	auto uniforms = WeatherUniformsOf(compiled->Definition, wrongTime, *grid);
	uniforms.Phase.z = 1;
	auto command = gpu.Acquire();
	REQUIRE(RecordWeatherGpuClock(gpu.Device, command.get(), gpu.Pipelines[0], gpu.Clock, sample));
	const std::array<glm::ivec4, 1> descriptor{};
	REQUIRE(RecordWeatherPages(gpu.Device, command.get(), pages, descriptor));
	REQUIRE(RecordWeatherField(command.get(), gpu.Pipelines[1], cells, pages, uniforms, 0, gpu.Clock.Buffer));
	gpu.Submit(std::move(command));
	const auto residentCells = gpu.Read(cells, 1024);
	CompareCells(residentCells, compiled->Definition, sample.Time, *grid);
	uniforms.Phase.z = 0;
	command = gpu.Acquire();
	REQUIRE(RecordWeatherField(command.get(), gpu.Pipelines[1], cells, pages, uniforms));
	gpu.Submit(std::move(command));
	const auto uniformCells = gpu.Read(cells, 1024);
	CompareCells(uniformCells, compiled->Definition, wrongTime, *grid);
	CHECK(residentCells != uniformCells);
	uniforms.Phase.z = UINT32_MAX;
	command = gpu.Acquire();
	REQUIRE(RecordWeatherField(command.get(), gpu.Pipelines[1], cells, pages, uniforms, 0, gpu.Clock.Buffer));
	gpu.Submit(std::move(command));
	const auto pendingCells = gpu.Read(cells, 1024);
	for (uint32_t index = 0; index < 512; ++index) {
		REQUIRE(pendingCells[index * 2] == 0x7f007f00u);
		REQUIRE(pendingCells[index * 2 + 1] == 0x007f7f7fu);
	}

	// Unsafe compact assemblies force summary evaluation to consult its clock binding.
	auto *compact = gpu.Buffer(2048, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
	auto *summaries = gpu.Buffer(16, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
	std::array<uint32_t, 512> words{};
	for (uint32_t group = 0; group < 64; ++group)
		words[group * 8 + 1] = 1u << 23;
	gpu.Upload(compact, words);
	uniforms = WeatherUniformsOf(compiled->Definition, wrongTime, *grid);
	uniforms.Origin.w = 0;
	uniforms.Phase.z = 1;
	uniforms.Phase.w = 1;
	uniforms.Bricks = {1, 1, 1, 1};
	const std::array<glm::ivec4, 1> compactPage{glm::ivec4{-8, 0, -8, 1 | (1 << 5)}};
	command = gpu.Acquire();
	REQUIRE(RecordWeatherPages(gpu.Device, command.get(), pages, compactPage));
	REQUIRE(RecordWeatherSummary(
		command.get(), gpu.Pipelines[2], compact, pages, summaries, uniforms, gpu.Clock.Buffer
	));
	gpu.Submit(std::move(command));
	const auto actualSummary = gpu.Read(summaries, 4);
	const auto expectedSummary = [&](const weather::WeatherTime &time) {
		std::array<uint32_t, 4> result{0, 255, 0, 0};
		for (uint32_t index = 0; index < 512; ++index) {
			const weather::CellCoordinate coordinate{
				-8 + int32_t(index & 7) * 2, int32_t((index >> 3) & 7) * 2, -8 + int32_t(index >> 6) * 2
			};
			const uint32_t density = weather::Evaluate(compiled->Definition, time, coordinate).Codes[0];
			result[0] = std::max(result[0], density);
			result[1] = std::min(result[1], density);
			result[2] += density;
			++result[3];
		}
		return result;
	};
	const auto expected = expectedSummary(sample.Time);
	REQUIRE(expected[0] > 0);
	REQUIRE(expected != expectedSummary(wrongTime));
	CHECK(std::equal(actualSummary.begin(), actualSummary.end(), expected.begin(), expected.end()));
}
