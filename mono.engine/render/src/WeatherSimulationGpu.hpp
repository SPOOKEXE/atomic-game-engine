#pragma once

// Private resident FP32 solver adapter. Checkpoint decode is transient; no CPU physical mirror survives
// upload.
#include "VulkanTimestamps.hpp"

#include <engine/weather/Checkpoint.hpp>
#include <engine/weather/Simulation.hpp>

#include <SDL3/SDL_gpu.h>

#include <functional>
#include <memory>

namespace engine::render {
	// Five read and five write buffers at sets 0 and 1. Terrain is immutable within a generation;
	// full precision cells and falling water publish together after diagnostic validation.
	struct alignas(16) SimulationGpuUniforms {
		std::array<uint32_t, 4> Grid{};
		std::array<uint32_t, 4> Boundaries{};
		std::array<float, 4> Integration{};
		std::array<float, 4> Phase{};
		std::array<float, 4> Thermal{};
		std::array<float, 4> Precipitation{};
		std::array<float, 4> Acceleration{};
		std::array<float, 4> Sources{};
		std::array<float, 4> OpenVelocity{};
		std::array<float, 4> WindRelaxation{};
		std::array<float, 4> InflowScalars{};
		std::array<float, 4> InflowVelocity{};
		std::array<float, 4> EncodingScalar{};
		std::array<float, 4> EncodingBackground{};
		std::array<float, 4> EncodingWind{};
		std::array<int32_t, 4> Origin{};
	};
	static_assert(sizeof(SimulationGpuUniforms) == 256);

	struct WeatherSimulationGpu {
		SDL_GPUBuffer *PublishedCells = nullptr;
		SDL_GPUBuffer *WorkingCells[2]{};
		SDL_GPUBuffer *ForcedCells = nullptr;
		SDL_GPUBuffer *ProjectedCells = nullptr;
		SDL_GPUBuffer *Scratch[2]{};
		SDL_GPUBuffer *Diagnostics = nullptr;
		SDL_GPUBuffer *PublishedPacked = nullptr;
		SDL_GPUBuffer *StagingPacked = nullptr;
		SDL_GPUBuffer *PublishedTerrain = nullptr;
		SDL_GPUBuffer *StagingTerrain = nullptr;
		SDL_GPUBuffer *PublishedRain = nullptr;
		SDL_GPUBuffer *WorkingRain[2]{};
		SDL_GPUTransferBuffer *Readback = nullptr;
		SDL_GPUFence *Fence = nullptr;
		weather::SimulationDefinition Definition;
		std::vector<weather::SimulationForcing> ForcingHistory;
		weather::SimulationDefinition PendingDefinition;
		std::vector<weather::SimulationForcing> PendingForcingHistory;
		uint64_t Tick = 0;
		uint64_t PendingTick = 0;
		uint64_t TerrainSignature = 0;
		uint64_t PendingTerrainSignature = 0;
		uint32_t CellCount = 0;
		uint32_t PendingCellIndex = 0;
		uint32_t PendingRainIndex = 0;
		bool PendingTerrain = false;
		bool Published = false;
		std::unique_ptr<VulkanTimestamps> Timings;
		uint32_t TimingSlot = VulkanTimestamps::NO_SLOT;
		bool TimingAttempted = false;
		uint32_t LastSubmittedDispatches = 0;
		uint32_t RecordedDispatches = 0;
		std::array<float, 11> PublishedEncoding{};
		std::array<float, 11> PendingEncoding{};
	};
	enum class WeatherSimulationRecordEvent : uint8_t { Begin, End, Cancel };
	using WeatherSimulationRecordHook =
		std::function<void(SDL_GPUCommandBuffer *, WeatherSimulationRecordEvent)>;
	// A separate submitted command owns each pending transition. Poll before sampling new output.
	[[nodiscard]] bool StageWeatherSimulationGpu(
		SDL_GPUDevice *device,
		SDL_GPUComputePipeline *pipeline,
		WeatherSimulationGpu &state,
		uint64_t targetTick,
		const weather::EncodingProfile &profile,
		std::span<const std::byte> checkpoint = {},
		uint32_t maximumReplayTicks = 128,
		const WeatherSimulationRecordHook &recordHook = {},
		const weather::SimulationDefinition *expectedDefinition = nullptr,
		bool boundReplayTarget = false
	);
	// Returns true only after a completed and validated transition. Never waits for GPU completion.
	[[nodiscard]] bool PollWeatherSimulationGpu(SDL_GPUDevice *device, WeatherSimulationGpu &state);
	void ReleaseWeatherSimulationGpu(SDL_GPUDevice *device, WeatherSimulationGpu &state);
	[[nodiscard]] SimulationGpuUniforms SimulationUniforms(
		const weather::SimulationDefinition &definition,
		const weather::SimulationForcing &forcing,
		uint32_t phase,
		float seconds,
		const weather::EncodingProfile &profile
	);
}
