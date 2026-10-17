#pragma once

#include "GraphHistory.hpp"

#include <engine/weather/Weather.hpp>

#include <array>
#include <cstdint>
#include <optional>

struct SDL_GPUBuffer;
struct SDL_GPUCommandBuffer;
struct SDL_GPUComputePipeline;
struct SDL_GPUDevice;

namespace engine::render {
	// The resident buffer uses the same two integer blocks as an absolute update.
	struct alignas(16) WeatherGpuClockUniforms {
		std::array<uint32_t, 4> Phase{};
		std::array<uint32_t, 4> Time{};
	};
	static_assert(sizeof(WeatherGpuClockUniforms) == 32);

	struct WeatherGpuClockSample {
		weather::WeatherTime Time;
		uint64_t SourceTick = 0;
		uint32_t TickRate = 0;
		uint64_t Identity = 0;
	};

	struct WeatherGpuClock {
		SDL_GPUBuffer *Buffer = nullptr;
		GraphHistoryGeneration Publication;
		WeatherGpuClockSample CommittedSample;
		WeatherGpuClockSample PendingSample;
		uint64_t AnchorTick = 0;
		uint64_t PendingAnchorTick = 0;
	};

	enum class WeatherGpuClockAction : uint8_t { Reuse, Anchor, Advance };

	struct WeatherGpuClockPlan {
		WeatherGpuClockAction Action = WeatherGpuClockAction::Reuse;
		WeatherGpuClockUniforms Uniforms;
		uint64_t AnchorTick = 0;
	};

	// Planning is device-free. A different command cannot consume an unpublished update.
	[[nodiscard]] std::optional<WeatherGpuClockPlan> PlanWeatherGpuClock(
		const WeatherGpuClock &clock, const WeatherGpuClockSample &sample, const void *command
	);
	[[nodiscard]] bool RecordWeatherGpuClock(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		WeatherGpuClock &clock,
		const WeatherGpuClockSample &sample
	);
	void CommitWeatherGpuClock(WeatherGpuClock &clock, const void *command);
	void DiscardWeatherGpuClock(WeatherGpuClock &clock, const void *command = nullptr);
	void ReleaseWeatherGpuClock(SDL_GPUDevice *device, WeatherGpuClock &clock);
}
