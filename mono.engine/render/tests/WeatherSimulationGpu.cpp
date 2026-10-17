#include "WeatherSimulationGpu.hpp"

#include "GpuHeap.hpp"
#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"

#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

TEST_SUITE_ID("engine.render.weathersimulationgpu")
TEST_DEPENDS("engine.weather.simulation")
TEST_DEPENDS("engine.weather.checkpoint")

namespace {
	using namespace engine;
	using namespace engine::render;

	struct SimulationDevice {
		test::FixtureDevice Fixture;
		SDL_GPUDevice *Device = nullptr;
		SDL_GPUComputePipeline *Pipeline = nullptr;
		WeatherSimulationGpu State;

		SimulationDevice() {
			Fixture.Initialise();
			Device = static_cast<SDL_GPUDevice *>(Fixture.Render.Backend().Device);
			const auto binary = ShaderBinaryFor(Device);
			std::ifstream file(resources::Shader("weather-evolve.comp", binary.Form), std::ios::binary);
			const std::vector<char> bytes{
				std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()
			};
			REQUIRE_FALSE(bytes.empty());
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = reinterpret_cast<const Uint8 *>(bytes.data());
			info.code_size = bytes.size();
			info.entrypoint = binary.EntryPoint;
			info.format = binary.Format;
			info.num_readonly_storage_buffers = 5;
			info.num_readwrite_storage_buffers = 5;
			info.num_uniform_buffers = 1;
			info.threadcount_x = 64;
			info.threadcount_y = 1;
			info.threadcount_z = 1;
			Pipeline = SDL_CreateGPUComputePipeline(Device, &info);
			REQUIRE(Pipeline);
		}

		~SimulationDevice() {
			ReleaseWeatherSimulationGpu(Device, State);
			if (Pipeline) SDL_ReleaseGPUComputePipeline(Device, Pipeline);
		}

		bool Finish() {
			bool accepted = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (State.Fence && std::chrono::steady_clock::now() < deadline) {
				accepted |= PollWeatherSimulationGpu(Device, State);
				if (State.Fence) SDL_Delay(1);
			}
			REQUIRE_FALSE(State.Fence);
			return accepted;
		}

		std::vector<std::byte> Read(SDL_GPUBuffer *buffer, uint32_t bytes) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = bytes;
			const auto releaseTransfer = [this](SDL_GPUTransferBuffer *buffer) {
				gpu::ReleaseTransferBuffer(Device, buffer);
			};
			std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
				gpu::CreateTransferBuffer(Device, &info), releaseTransfer
			);
			REQUIRE(transfer);
			auto *command = SDL_AcquireGPUCommandBuffer(Device);
			REQUIRE(command);
			auto *copy = SDL_BeginGPUCopyPass(command);
			REQUIRE(copy);
			const SDL_GPUBufferRegion source{buffer, 0, bytes};
			const SDL_GPUTransferBufferLocation destination{transfer.get(), 0};
			SDL_DownloadFromGPUBuffer(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			const auto releaseFence = [this](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(Device, fence); };
			std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
				SDL_SubmitGPUCommandBufferAndAcquireFence(command), releaseFence
			);
			REQUIRE(fence);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!SDL_QueryGPUFence(Device, fence.get()) && std::chrono::steady_clock::now() < deadline)
				SDL_Delay(1);
			REQUIRE(SDL_QueryGPUFence(Device, fence.get()));
			const void *mapped = SDL_MapGPUTransferBuffer(Device, transfer.get(), false);
			REQUIRE(mapped);
			std::vector<std::byte> result(bytes);
			std::memcpy(result.data(), mapped, bytes);
			SDL_UnmapGPUTransferBuffer(Device, transfer.get());
			return result;
		}
	};

	weather::SimulationState Snapshot(weather::SolverBoundary horizontal, bool terrain) {
		weather::SimulationDefinition definition;
		definition.Grid.Cells = {4, 4, 4};
		definition.Grid.CellSizeMetres = 8;
		definition.Grid.Boundaries = {
			horizontal, weather::SolverBoundary::GroundOpen, weather::SolverBoundary::Periodic
		};
		definition.MaximumVelocityMetresPerSecond = 8;
		definition.MaximumPressureGradientPascalsPerMetre = 0;
		definition.DiffusionSquareMetresPerSecond = 0;
		definition.BuoyancyMetresPerSecondSquaredPerCelsius = 0;
		definition.PhaseRelaxationPerSecond = 0;
		definition.PrecipitationPerSecond = .5f;
		definition.RainFallSpeedMetresPerSecond = 2;
		definition.GroundVapourFluxKilogramsPerSquareMetreSecond = .0004f;
		definition.ProjectionIterations = 256;
		weather::SolverCell initial;
		initial.VapourKilogramsPerKilogram = .005f;
		initial.CondensateKilogramsPerCubicMetre = .004f;
		initial.VelocityMetresPerSecond = horizontal == weather::SolverBoundary::Closed
											  ? std::array<float, 3>{0, 0, 0}
											  : std::array<float, 3>{1, 0, 0};
		weather::SimulationForcing forcing;
		forcing.Inflow = initial;
		forcing.OpenVelocityMetresPerSecond = initial.VelocityMetresPerSecond;
		std::vector<float> heights(16, weather::NO_TERRAIN_HEIGHT);
		if (terrain)
			for (uint32_t z = 0; z < 4; ++z) {
				heights[4 * z] = 8;
				heights[1 + 4 * z] = 0;
				heights[3 + 4 * z] = 24;
			}
		auto state = weather::CreateSimulation(definition, initial, forcing, heights);
		REQUIRE(state);
		for (size_t index = 0; index < state->Cells.size(); ++index)
			if (!(weather::SimulationCellFlags(*state, index) & 1) && (index / 4) % 4 == 3)
				state->FallingCondensateKilogramsPerCubicMetre[index] = .02f;
		state->Budget.InitialWaterKilograms = weather::SimulationWaterKilograms(*state);
		REQUIRE(weather::ValidateSimulation(*state));
		return *state;
	}

	void ComparePhysical(SimulationDevice &gpu, const weather::SimulationState &expected) {
		const auto cells =
			gpu.Read(gpu.State.PublishedCells, uint32_t(expected.Cells.size() * sizeof(weather::SolverCell)));
		const auto rain = gpu.Read(gpu.State.PublishedRain, uint32_t(expected.Cells.size() * sizeof(float)));
		const auto masks =
			gpu.Read(gpu.State.PublishedTerrain, uint32_t(expected.Cells.size() * sizeof(uint32_t)));
		const auto packed = gpu.Read(gpu.State.PublishedPacked, uint32_t(expected.Cells.size() * 8));
		weather::EncodingProfile profile;
		profile.CellSizeMetres = expected.Definition.Grid.CellSizeMetres;
		const auto referenceCodes = weather::PackSimulation(expected, profile);
		REQUIRE(referenceCodes);
		double water = 0;
		for (size_t index = 0; index < expected.Cells.size(); ++index) {
			INFO("cell=" << index);
			weather::SolverCell actual;
			std::memcpy(&actual, cells.data() + index * sizeof(actual), sizeof(actual));
			CHECK(
				std::abs(
					actual.VapourKilogramsPerKilogram - expected.Cells[index].VapourKilogramsPerKilogram
				) <= .000002f
			);
			CHECK(
				std::abs(
					actual.CondensateKilogramsPerCubicMetre -
					expected.Cells[index].CondensateKilogramsPerCubicMetre
				) <= .000002f
			);
			CHECK(std::abs(actual.TemperatureCelsius - expected.Cells[index].TemperatureCelsius) <= .005f);
			CHECK(
				std::abs(
					actual.AtmosphericPressurePascals - expected.Cells[index].AtmosphericPressurePascals
				) <= .1f
			);
			for (size_t axis = 0; axis < 3; ++axis)
				CHECK(
					std::abs(
						actual.VelocityMetresPerSecond[axis] -
						expected.Cells[index].VelocityMetresPerSecond[axis]
					) <= .002f
				);
			CHECK(
				std::abs(
					actual.ProjectionPressurePascals - expected.Cells[index].ProjectionPressurePascals
				) <= .5f
			);
			float actualRain;
			uint32_t actualMask;
			std::memcpy(&actualRain, rain.data() + index * sizeof(float), sizeof(float));
			std::memcpy(&actualMask, masks.data() + index * sizeof(uint32_t), sizeof(uint32_t));
			CHECK(std::abs(actualRain - expected.FallingCondensateKilogramsPerCubicMetre[index]) <= .000002f);
			CHECK(actualMask == weather::SimulationCellFlags(expected, index));
			if (!(actualMask & 1))
				water +=
					(actual.VapourKilogramsPerKilogram * expected.Definition.DryAirKilogramsPerCubicMetre +
					 actual.CondensateKilogramsPerCubicMetre + actualRain);
			for (size_t lane = 0; lane < 8; ++lane) {
				const auto code = std::to_integer<uint8_t>(packed[index * 8 + lane]);
				const int tolerance = actualMask & 1 ? 0 : 1;
				CHECK(std::abs(int(code) - int((*referenceCodes)[index].Codes[lane])) <= tolerance);
				if (lane == 1 || (lane >= 3 && lane <= 6)) CHECK(code != 255);
			}
		}
		const double size = expected.Definition.Grid.CellSizeMetres;
		water *= size * size * size;
		const double referenceWater = weather::SimulationWaterKilograms(expected);
		CHECK(std::abs(water - referenceWater) <= std::max(.0001, referenceWater * .00002));
	}
}

TEST_CASE(
	"stateful GPU uniforms carry falling speed and ground flux without changing cell ABI",
	"[render][weather-stateful]"
) {
	weather::SimulationDefinition definition;
	definition.RainFallSpeedMetresPerSecond = 7;
	definition.GroundVapourFluxKilogramsPerSquareMetreSecond = .002f;
	const auto uniforms = SimulationUniforms(definition, {}, 4, .05f, {});
	CHECK(uniforms.Precipitation[3] == 7);
	CHECK(uniforms.Sources[2] == .002f);
	CHECK(sizeof(weather::SolverCell) == 32);
	CHECK(sizeof(SimulationGpuUniforms) == 256);
}

TEST_CASE(
	"stateful GPU terrain falling water and boundary evolution agree with CPU",
	"[render][gpu][weather-stateful][.]"
) {
	for (auto boundary : {
			 weather::SolverBoundary::Periodic, weather::SolverBoundary::Closed, weather::SolverBoundary::Open
		 }) {
		CAPTURE(boundary);
		SimulationDevice gpu;
		auto expected = Snapshot(boundary, true);
		const auto checkpoint = weather::EncodeCheckpoint(expected);
		REQUIRE(checkpoint);
		weather::EncodingProfile profile;
		profile.CellSizeMetres = expected.Definition.Grid.CellSizeMetres;
		REQUIRE(StageWeatherSimulationGpu(
			gpu.Device, gpu.Pipeline, gpu.State, 0, profile, *checkpoint, 128, {}, &expected.Definition
		));
		CHECK_FALSE(gpu.State.Published);
		REQUIRE(gpu.Finish());
		ComparePhysical(gpu, expected);
		for (uint64_t tick = 1; tick <= 4; ++tick) {
			REQUIRE(weather::StepSimulation(expected) == weather::SimulationStatus::Success);
			REQUIRE(StageWeatherSimulationGpu(
				gpu.Device, gpu.Pipeline, gpu.State, tick, profile, {}, 128, {}, &expected.Definition
			));
			REQUIRE(gpu.Finish());
			ComparePhysical(gpu, expected);
		}
		CHECK(expected.Budget.PrecipitationWaterKilograms > 0);
		const auto submissions = gpu.State.LastSubmittedDispatches;
		CHECK(submissions > 0);
		REQUIRE(StageWeatherSimulationGpu(
			gpu.Device, gpu.Pipeline, gpu.State, 4, profile, {}, 128, {}, &expected.Definition
		));
		CHECK_FALSE(gpu.State.Fence);
		CHECK(gpu.State.LastSubmittedDispatches == 0);
	}
}

TEST_CASE(
	"stateful GPU Escape top transports upward moisture with a closed lower face",
	"[render][gpu][weather-stateful][.]"
) {
	SimulationDevice gpu;
	auto expected = Snapshot(weather::SolverBoundary::Open, false);
	auto forcing = expected.ForcingHistory.back();
	forcing.OpenVelocityMetresPerSecond[1] = 1;
	forcing.AccelerationMetresPerSecondSquared[1] = .25f;
	forcing.Inflow.VapourKilogramsPerKilogram = 0;
	forcing.Inflow.CondensateKilogramsPerCubicMetre = 0;
	REQUIRE(weather::AddSimulationForcing(expected, forcing));
	const auto checkpoint = weather::EncodeCheckpoint(expected);
	REQUIRE(checkpoint);
	weather::EncodingProfile profile;
	profile.CellSizeMetres = expected.Definition.Grid.CellSizeMetres;
	REQUIRE(StageWeatherSimulationGpu(
		gpu.Device, gpu.Pipeline, gpu.State, 0, profile, *checkpoint, 128, {}, &expected.Definition
	));
	REQUIRE(gpu.Finish());
	const double initialWater = weather::SimulationWaterKilograms(expected);
	for (uint64_t tick = 1; tick <= 4; ++tick) {
		REQUIRE(weather::StepSimulation(expected) == weather::SimulationStatus::Success);
		REQUIRE(StageWeatherSimulationGpu(
			gpu.Device, gpu.Pipeline, gpu.State, tick, profile, {}, 128, {}, &expected.Definition
		));
		REQUIRE(gpu.Finish());
		ComparePhysical(gpu, expected);
	}
	float upwardTopFlow = 0;
	for (uint32_t z = 0; z < 4; ++z)
		for (uint32_t x = 0; x < 4; ++x)
			upwardTopFlow += expected.Cells[x + 4 * (3 + 4 * z)].VelocityMetresPerSecond[1];
	CHECK(upwardTopFlow > 0);
	CHECK(expected.Budget.BoundaryWaterKilograms < 0);
	CHECK(weather::SimulationWaterKilograms(expected) < initialWater);
}

TEST_CASE(
	"rejected stateful GPU trials retain published cells rain and terrain",
	"[render][gpu][weather-stateful][.]"
) {
	SimulationDevice gpu;
	auto original = Snapshot(weather::SolverBoundary::Periodic, false);
	const auto checkpoint = weather::EncodeCheckpoint(original);
	REQUIRE(checkpoint);
	weather::EncodingProfile profile;
	profile.CellSizeMetres = 8;
	REQUIRE(StageWeatherSimulationGpu(gpu.Device, gpu.Pipeline, gpu.State, 0, profile, *checkpoint));
	REQUIRE(gpu.Finish());
	const auto cells = gpu.Read(gpu.State.PublishedCells, 64 * 32);
	const auto rain = gpu.Read(gpu.State.PublishedRain, 64 * 4);
	const auto terrain = gpu.Read(gpu.State.PublishedTerrain, 64 * 4);
	weather::SolverCell geometryInitial;
	weather::SimulationForcing geometryForcing;
	geometryForcing.Inflow = geometryInitial;
	const std::array<float, 16> replacementHeights{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
	const auto sameRevisionTerrain =
		weather::CreateSimulation(original.Definition, geometryInitial, geometryForcing, replacementHeights);
	REQUIRE(sameRevisionTerrain);
	const auto sameRevisionCheckpoint = weather::EncodeCheckpoint(*sameRevisionTerrain);
	REQUIRE(sameRevisionCheckpoint);
	CHECK_FALSE(
		StageWeatherSimulationGpu(gpu.Device, gpu.Pipeline, gpu.State, 0, profile, *sameRevisionCheckpoint)
	);
	CHECK_FALSE(gpu.State.Fence);
	auto definition = original.Definition;
	definition.MaximumVelocityMetresPerSecond = .01f;
	definition.TerrainRevision++;
	weather::SolverCell initial;
	initial.VapourKilogramsPerKilogram = .005f;
	initial.CondensateKilogramsPerCubicMetre = .004f;
	weather::SimulationForcing forcing;
	forcing.AccelerationMetresPerSecondSquared[0] = 256;
	forcing.Inflow = initial;
	const std::array<float, 16> heights{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
	const auto trial = weather::CreateSimulation(definition, initial, forcing, heights);
	REQUIRE(trial);
	const auto trialCheckpoint = weather::EncodeCheckpoint(*trial);
	REQUIRE(trialCheckpoint);
	REQUIRE(StageWeatherSimulationGpu(
		gpu.Device, gpu.Pipeline, gpu.State, 1, profile, *trialCheckpoint, 128, {}, &definition
	));
	CHECK_FALSE(gpu.Finish());
	CHECK(gpu.State.Tick == 0);
	CHECK(gpu.State.Definition == original.Definition);
	CHECK(gpu.Read(gpu.State.PublishedCells, 64 * 32) == cells);
	CHECK(gpu.Read(gpu.State.PublishedRain, 64 * 4) == rain);
	CHECK(gpu.Read(gpu.State.PublishedTerrain, 64 * 4) == terrain);
	CHECK_FALSE(StageWeatherSimulationGpu(
		gpu.Device, gpu.Pipeline, gpu.State, 1, profile, *checkpoint, 128, {}, &definition
	));
	CHECK_FALSE(gpu.State.Fence);
}
