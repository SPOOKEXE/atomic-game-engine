#pragma once

// Private packed weather ABI. Canonical samples remain owned by Engine::weather;
// the graph owns the resident field and its conservative brick summaries.

#include "GraphHistory.hpp"
#include "WeatherResidency.hpp"
#include "WeatherShadows.hpp"
#include "WeatherSimulationGpu.hpp"

#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/weather/Weather.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <optional>

namespace engine::render {
	inline constexpr uint32_t WEATHER_MAX_CELLS = 262144;
	inline constexpr uint32_t WEATHER_MAX_BRICKS = 512;
	inline constexpr uint32_t WEATHER_BRICK_EDGE = 8;

	struct WeatherGpuGrid {
		weather::CellCoordinate Origin;
		glm::uvec3 Dimensions{};
		uint32_t Stride = 1;
		glm::uvec3 BrickDimensions{};
		uint32_t CellCount = 0;
		uint32_t BrickCount = 0;
	};

	// Integer lanes are explicit so SPIR-V and translated MSL consume identical words.
	struct WeatherGpuUniforms {
		glm::uvec4 Grid;
		glm::ivec4 Origin;
		glm::uvec4 Identity;
		glm::uvec4 Shape;
		glm::uvec4 Moisture;
		glm::ivec4 Dynamics;
		glm::ivec4 Wind;
		glm::uvec4 Time;
		glm::uvec4 Phase;
		glm::uvec4 Bricks;
		glm::ivec4 Thermodynamics;
	};
	static_assert(sizeof(WeatherGpuUniforms) == 176);

	struct WeatherRayUniforms {
		glm::mat4 InverseViewProjection{1.0f};
		glm::vec4 Eye;
		glm::vec4 Minimum;
		glm::vec4 Maximum;
		glm::vec4 ColourExtinction;
		glm::vec4 Sun;
		glm::vec4 SunColour;
		glm::vec4 Ambient;
		glm::uvec4 Quality;
		WeatherGpuUniforms Field;
	};
	static_assert(sizeof(WeatherRayUniforms) == 368);

	struct WeatherGpuLighting {
		glm::vec4 Direction;
		glm::vec4 Direct;
		glm::vec4 Ambient;
	};
	[[nodiscard]] WeatherGpuLighting WeatherLightingOf(const scene::WorldLighting &lighting);

	struct WeatherGpuWorldView {
		uint64_t World = 0;
		WeatherResidencyView Demand;
	};

	struct WeatherGpuInputs {
		WeatherGpuUniforms Uniforms;
		weather::CompiledWeather Compiled;
		weather::WeatherTime Time;
		bool Stateful = false;
	};

	struct WeatherPhysicalOwner {
		uint64_t World = 0;
		uint64_t RecipeSignature = 0;
		WeatherSimulationGpu Physical;
		uint64_t CheckpointSignature = 0;
		uint64_t EncodingSignature = 0;
		uint64_t PhysicalRecipeSignature = 0;
		uint64_t PendingPhysicalRecipeSignature = 0;
		uint64_t PendingEncodingSignature = 0;
		uint64_t PendingCheckpointSignature = 0;
		bool CatchupPending = false;
		uint64_t PendingStageSignature = 0;
		std::optional<uint64_t> RefusedStageSignature;
		std::optional<weather::SimulationDefinition> ExpectedDefinition;
	};

	struct WeatherGpuGeneration {
		uint64_t World = 0;
		core::Name Pipeline;
		SDL_GPUBuffer *Cells = nullptr;
		SDL_GPUBuffer *Summaries = nullptr;
		SDL_GPUBuffer *Pages = nullptr;
		SDL_GPUBuffer *PreviousPages = nullptr;
		uint32_t FieldWords = 0;
		float TransitionAlpha = 1.0f;
		uint32_t PreviousPageCount = 0;
		GraphHistoryGeneration Residency;
		WeatherShadowCache Shadows;
		WeatherResidencyCache Topology;
		// Stable private world ownership shares one solver transaction across graph pipelines.
		WeatherPhysicalOwner *PhysicalOwner = nullptr;
		std::optional<WeatherGpuInputs> RenderInputs;
		std::optional<WeatherGpuInputs> PendingRenderInputs;

		const WeatherGpuInputs *InputsFor(const void *command) const {
			if (Field.Pending && Field.PendingCommand == command && PendingRenderInputs)
				return &*PendingRenderInputs;
			return RenderInputs ? &*RenderInputs : nullptr;
		}
		GraphHistoryGeneration Field;
		GraphHistoryGeneration Summary;
	};

	[[nodiscard]] std::optional<WeatherGpuGrid> WeatherGridOf(const scene::WeatherCompute &controls);
	[[nodiscard]] std::optional<WeatherGpuGrid>
	WeatherSimulationGridOf(const scene::WeatherCompute &controls);
	[[nodiscard]] std::optional<WeatherGpuGrid>
	WeatherSimulationGridOf(const weather::SimulationDefinition &definition);
	[[nodiscard]] weather::EncodingProfile WeatherEncodingOf(const scene::WeatherCompute &controls);
	[[nodiscard]] WeatherGpuUniforms WeatherUniformsOf(
		const weather::WeatherDefinition &definition,
		const weather::WeatherTime &time,
		const WeatherGpuGrid &grid
	);
	[[nodiscard]] uint64_t WeatherSignature(const WeatherGpuUniforms &uniforms);
	[[nodiscard]] uint64_t WeatherEncodingSignature(const weather::EncodingProfile &profile);
	[[nodiscard]] uint64_t
	WeatherPhysicalSignature(const scene::Environment &environment, core::Name simulationLayer = {});
	[[nodiscard]] bool RecordWeatherPages(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		SDL_GPUBuffer *pages,
		std::span<const glm::ivec4> keys
	);
	[[nodiscard]] bool RecordWeatherField(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		const WeatherGpuUniforms &uniforms,
		uint64_t physicalBytes = 0
	);
	[[nodiscard]] bool
	RecordWeatherEmptyField(SDL_GPUDevice *device, SDL_GPUCommandBuffer *command, SDL_GPUBuffer *cells);
	[[nodiscard]] bool RecordWeatherSummary(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *summaries,
		const WeatherGpuUniforms &uniforms
	);
	[[nodiscard]] bool RecordWeatherRays(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *summaries,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *previousPages,
		SDL_GPUTexture *source,
		SDL_GPUTexture *depth,
		SDL_GPUTexture *destination,
		SDL_GPUTexture *cloudDepth,
		SDL_GPUSampler *sampler,
		uint32_t width,
		uint32_t height,
		const WeatherRayUniforms &uniforms
	);
}
