#pragma once

// Private packed weather ABI. Canonical samples remain owned by Engine::weather;
// the graph owns the resident field and its conservative brick summaries.

#include "GpuCommandIdentity.hpp"
#include "GraphHistory.hpp"
#include "WeatherGpuClock.hpp"
#include "WeatherResidency.hpp"
#include "WeatherShadows.hpp"
#include "WeatherSimulationGpu.hpp"

#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/weather/Weather.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

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

	// The horizontal span clouds are drawn over, in world metres, as minimum X, minimum
	// Z, maximum X and maximum Z: the authored extent, or for a horizon layer
	// (`scene::PresentsHorizon`) twice the horizon distance centred on `centre`.
	glm::vec4 WeatherHorizontalSpan(const scene::WeatherCompute &controls, const glm::vec3 &centre);

	// Cloud rays one integration may march: a half-resolution 1280 by 720 view.
	inline constexpr uint64_t WEATHER_RAY_BUDGET = 640 * 360;

	// The part of a `width` by `height` cloud target the integrator marches:
	// all of it within the budget, otherwise the largest corner of the same
	// aspect that fits. Larger windows march the same number of rays, and the
	// reconstruction upsamples from fewer of them, so cloud cost stops growing
	// with window size.
	glm::uvec2 WeatherRayExtent(uint32_t width, uint32_t height);

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
		// Active hardware-depth rectangle. Zero uses the complete bound texture.
		// z is the `CloudComputeShader` presentation profile.
		glm::uvec4 ImageExtent{};
		// The cloud shadow map's lighting state, so each cloud sample is lit by
		// the sun transmittance the shadow pass already cached. Zero cascades
		// (no shadow map this frame) leaves every sample fully lit.
		WeatherShadowLightingUniforms SunShadow{};
		// The baked density box (`WeatherBake`): canonical cell origin and cell
		// size, then the voxel counts in use and 1 when the march reads it.
		glm::vec4 BakeOrigin{};
		glm::vec4 BakeVoxels{};
		// Clipmap levels (`WeatherBake::Detail`), finest first: origin in the
		// level's own voxels and its voxel size, then the voxel counts in use
		// and 1 when in use.
		glm::vec4 DetailOrigin[3]{};
		glm::vec4 DetailVoxels[3]{};
	};
	static_assert(sizeof(WeatherRayUniforms) == 624);

	// Raw density baked from a world's field into a 3D texture covering the
	// cloud band, one voxel per canonical cell. The march reads one filtered
	// texel per sample here instead of decoding eight field corners, which is
	// almost all of its cost. Samples outside the box read the field directly.
	struct WeatherBake {
		// Voxels the texture holds; a box may use fewer.
		static constexpr glm::uvec3 CAPACITY{512, 16, 512};
		SDL_GPUTexture *Texture = nullptr;
		// Clipmap levels: finished density around the camera, smooth across
		// cells and with the preset's sub-cell detail baked in, so near clouds
		// show more than the field's 64 m cells. Level 0 has four voxels per
		// cell (about 4 km across); each next level doubles the voxel and the
		// span, so the last is one voxel per cell (about 16 km), smooth where
		// `Texture`'s trilinear shows the cells as blocks. Farther samples read
		// `Texture`. See `weather-detail.comp`.
		static constexpr size_t DETAIL_LEVELS = 3;
		static constexpr glm::uvec3 DETAIL_CAPACITY{256, 64, 256};
		static constexpr float DETAIL_PER_CELL = 4.0f;
		std::array<SDL_GPUTexture *, DETAIL_LEVELS> Detail{};
	};

	// The box a bake covers for a march with `uniforms`: the cloud band from
	// `Minimum` to `Maximum`, in whole canonical cells, centred on the eye when
	// the band is wider than the texture. Fills `BakeOrigin` and `BakeVoxels`,
	// and `DetailOrigin` and `DetailVoxels` for each clipmap level: the layer
	// around the eye, inside the coarse box.
	void PlanWeatherBake(WeatherRayUniforms &uniforms);

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
		WeatherGpuClock Clock;
		SDL_GPUBuffer *Cells = nullptr;
		SDL_GPUBuffer *Summaries = nullptr;
		SDL_GPUBuffer *Pages = nullptr;
		SDL_GPUBuffer *PreviousPages = nullptr;
		uint32_t FieldWords = 0;
		float TransitionAlpha = 1.0f;
		uint32_t PreviousPageCount = 0;
		GraphHistoryGeneration Residency;
		WeatherShadowCache Shadows;
		WeatherBake Bake;
		WeatherResidencyCache Topology;
		// Stable private world ownership shares one solver transaction across graph pipelines.
		WeatherPhysicalOwner *PhysicalOwner = nullptr;
		std::optional<WeatherGpuInputs> RenderInputs;
		std::optional<WeatherGpuInputs> PendingRenderInputs;

		const WeatherGpuInputs *InputsFor(const void *command) const {
			if (Field.Pending && Field.PendingCommand == GpuCommandIdentity(command) && PendingRenderInputs)
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
	[[nodiscard]] uint64_t WeatherPhysicalSignature(
		const scene::Environment &environment, core::Name simulationLayer = {}, uint32_t terrainRevision = 0
	);
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
		uint64_t physicalBytes = 0,
		SDL_GPUBuffer *clock = nullptr
	);
	[[nodiscard]] bool
	RecordWeatherEmptyField(SDL_GPUDevice *device, SDL_GPUCommandBuffer *command, SDL_GPUBuffer *cells);
	[[nodiscard]] bool RecordWeatherSummary(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *summaries,
		const WeatherGpuUniforms &uniforms,
		SDL_GPUBuffer *clock = nullptr
	);
	[[nodiscard]] bool RecordWeatherRays(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *summaries,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *previousPages,
		SDL_GPUTexture *depth,
		SDL_GPUTexture *destination,
		SDL_GPUTexture *cloudDepth,
		SDL_GPUSampler *sampler,
		uint32_t width,
		uint32_t height,
		const WeatherRayUniforms &uniforms,
		SDL_GPUBuffer *clock = nullptr,
		SDL_GPUTexture *sunShadow = nullptr,
		SDL_GPUTexture *baked = nullptr,
		std::span<SDL_GPUTexture *const> detail = {}
	);
	// Writes the box `uniforms.BakeOrigin`/`BakeVoxels` describe into `baked`.
	[[nodiscard]] bool RecordWeatherBake(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *summaries,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *previousPages,
		SDL_GPUTexture *baked,
		const WeatherRayUniforms &uniforms,
		SDL_GPUBuffer *clock = nullptr
	);
	// Writes clipmap `level` (`uniforms.DetailOrigin`/`DetailVoxels`) into
	// `detail` from the coarse bake `baked`.
	[[nodiscard]] bool RecordWeatherDetail(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUTexture *baked,
		SDL_GPUTexture *detail,
		SDL_GPUSampler *sampler,
		const WeatherRayUniforms &uniforms,
		size_t level
	);
	// Records every clipmap level of `bake` with `pipeline` (null skips them),
	// switching off in `uniforms` each level that could not be recorded.
	// Returns the dispatches recorded.
	size_t RecordWeatherDetails(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		const WeatherBake &bake,
		SDL_GPUSampler *sampler,
		WeatherRayUniforms &uniforms
	);
	// Makes the bake textures if they are missing. False when the device refuses.
	[[nodiscard]] bool EnsureWeatherBake(SDL_GPUDevice *device, WeatherBake &bake);
	void ReleaseWeatherBake(SDL_GPUDevice *device, WeatherBake &bake);
}
