#include "WeatherGpu.hpp"

#include "GpuHeap.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>

namespace engine::render {
	std::optional<WeatherGpuGrid> WeatherGridOf(const scene::WeatherCompute &controls) {
		if (!weather::Compile(controls) || controls.MaximumResidentBricks == 0) return std::nullopt;
		WeatherGpuGrid grid;
		const double cell = controls.CellSizeMetres;
		const auto coordinate = [cell](double metres) -> std::optional<int32_t> {
			const double value = std::floor(metres / cell);
			if (value < INT32_MIN || value > INT32_MAX) return std::nullopt;
			return static_cast<int32_t>(value);
		};
		const auto x = coordinate(-double(controls.ExtentMetres.X) * .5);
		const auto y = coordinate(controls.BaseHeightMetres);
		const auto z = coordinate(-double(controls.ExtentMetres.Z) * .5);
		if (!x || !y || !z) return std::nullopt;
		grid.Origin = {*x, *y, *z};
		const double spans[3]{controls.ExtentMetres.X, controls.ThicknessMetres, controls.ExtentMetres.Z};
		const uint32_t maximumBricks = std::min(controls.MaximumResidentBricks, WEATHER_MAX_BRICKS);
		for (uint32_t attempt = 0; attempt < 31; ++attempt) {
			uint64_t cells = 1;
			uint64_t bricks = 1;
			for (uint32_t axis = 0; axis < 3; ++axis) {
				const double count = std::ceil(spans[axis] / (cell * grid.Stride));
				if (!(count >= 1 && count <= INT32_MAX)) return std::nullopt;
				grid.Dimensions[axis] = static_cast<uint32_t>(count);
				grid.BrickDimensions[axis] = (grid.Dimensions[axis] + 7) / 8;
				cells = std::min(uint64_t(WEATHER_MAX_CELLS) + 1, cells * grid.Dimensions[axis]);
				bricks = std::min(uint64_t(WEATHER_MAX_BRICKS) + 1, bricks * grid.BrickDimensions[axis]);
				const int64_t last = int64_t(
										 axis == 0	 ? *x
										 : axis == 1 ? *y
													 : *z
									 ) +
									 int64_t(grid.Dimensions[axis] - 1) * grid.Stride;
				if (last > INT32_MAX) return std::nullopt;
			}
			if (cells <= WEATHER_MAX_CELLS && bricks <= maximumBricks) {
				grid.CellCount = static_cast<uint32_t>(cells);
				grid.BrickCount = static_cast<uint32_t>(bricks);
				return grid;
			}
			grid.Stride *= 2;
		}
		return std::nullopt;
	}

	std::optional<WeatherGpuGrid> WeatherSimulationGridOf(const scene::WeatherCompute &controls) {
		scene::WeatherCompute physicalControls = controls;
		physicalControls.Paused = false;
		physicalControls.TimeScale = 1;
		const auto definition =
			weather::MakeSimulationDefinition(physicalControls, std::string(controls.LayerName.Text()));
		if (!definition) return std::nullopt;
		return WeatherSimulationGridOf(*definition);
	}

	std::optional<WeatherGpuGrid> WeatherSimulationGridOf(const weather::SimulationDefinition &definition) {
		if (!std::isfinite(definition.Grid.CellSizeMetres) || definition.Grid.CellSizeMetres <= 0)
			return std::nullopt;
		for (uint32_t extent : definition.Grid.Cells)
			if (!extent || extent > 128) return std::nullopt;
		if (uint64_t(definition.Grid.Cells[0]) * definition.Grid.Cells[1] * definition.Grid.Cells[2] > 65536)
			return std::nullopt;
		WeatherGpuGrid grid;
		grid.Origin = {definition.Grid.Origin[0], definition.Grid.Origin[1], definition.Grid.Origin[2]};
		grid.Dimensions = {definition.Grid.Cells[0], definition.Grid.Cells[1], definition.Grid.Cells[2]};
		grid.Stride = 1;
		grid.BrickDimensions = (grid.Dimensions + glm::uvec3(7)) / glm::uvec3(8);
		grid.CellCount = grid.Dimensions.x * grid.Dimensions.y * grid.Dimensions.z;
		grid.BrickCount = grid.BrickDimensions.x * grid.BrickDimensions.y * grid.BrickDimensions.z;
		if (grid.CellCount > WEATHER_MAX_CELLS || grid.BrickCount > WEATHER_MAX_BRICKS) return std::nullopt;
		return grid;
	}

	weather::EncodingProfile WeatherEncodingOf(const scene::WeatherCompute &controls) {
		weather::EncodingProfile profile;
		profile.CellSizeMetres = controls.CellSizeMetres;
		profile.CondensateKilogramsPerCubicMetre = controls.CondensateScale;
		profile.VapourKilogramsPerKilogram = controls.VapourScale;
		profile.BaseTemperatureCelsius = controls.BaseTemperatureCelsius;
		profile.BasePressureHectopascals = controls.BasePressureHectopascals;
		profile.TemperatureLapseCelsiusPerKilometre = controls.TemperatureLapseCelsiusPerKilometre;
		return profile;
	}

	WeatherGpuLighting WeatherLightingOf(const scene::WorldLighting &lighting) {
		WeatherGpuLighting result;
		const auto &direction = lighting.Direction;
		const float length =
			std::sqrt(direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z);
		result.Direction =
			length > 0 && std::isfinite(length)
				? glm::vec4{direction.X / length, direction.Y / length, direction.Z / length, 0}
				: glm::vec4{0, -1, 0, 0};
		result.Direct = {lighting.Direct.R, lighting.Direct.G, lighting.Direct.B, 0};
		// The cloud volume receives the base ambient and the average hemispheric sky contribution.
		result.Ambient = {
			lighting.Ambient.R + lighting.OutdoorAmbient.R * .5f,
			lighting.Ambient.G + lighting.OutdoorAmbient.G * .5f,
			lighting.Ambient.B + lighting.OutdoorAmbient.B * .5f,
			0
		};
		return result;
	}

	WeatherGpuUniforms WeatherUniformsOf(
		const weather::WeatherDefinition &definition,
		const weather::WeatherTime &time,
		const WeatherGpuGrid &grid
	) {
		return {
			.Grid = {grid.Dimensions, grid.CellCount},
			.Origin = {grid.Origin.X, grid.Origin.Y, grid.Origin.Z, int32_t(grid.Stride)},
			.Identity = {definition.Seed, definition.LayerKey, definition.Revision, definition.Version},
			.Shape =
				{uint32_t(definition.BaseCell),
				 definition.ThicknessCells,
				 definition.FeatureShift,
				 definition.Coverage},
			.Moisture =
				{definition.Vapour,
				 definition.TemperatureVariation,
				 definition.PressureVariation,
				 definition.Turbulence},
			.Dynamics =
				{definition.Shear,
				 definition.Lift,
				 int32_t(definition.FrontContrast),
				 int32_t(definition.Growth)},
			.Wind =
				{definition.WindCodes[0],
				 definition.WindCodes[1],
				 definition.WindCodes[2],
				 int32_t(definition.Ice)},
			.Time = {time.DisplacementQ8[0], time.DisplacementQ8[1], time.DisplacementQ8[2], time.Revision},
			.Phase = {uint32_t(time.PhaseTick), uint32_t(time.PhaseTick >> 32), 0, 0},
			.Bricks = {grid.BrickDimensions, grid.BrickCount},
			.Thermodynamics = {
				definition.SaturationBase,
				definition.SaturationLapseQ8,
				int32_t(definition.TemperatureSaturationQ8),
				0
			},
		};
	}

	uint64_t WeatherSignature(const WeatherGpuUniforms &uniforms) {
		uint64_t signature = 1;
		for (const auto byte : std::as_bytes(std::span(&uniforms, 1)))
			signature = scene::MixSignature(signature, std::to_integer<uint8_t>(byte));
		return signature;
	}

	uint64_t WeatherEncodingSignature(const weather::EncodingProfile &profile) {
		uint64_t signature = 1;
		for (float value :
			 {profile.CellSizeMetres,
			  profile.CondensateKilogramsPerCubicMetre,
			  profile.VapourKilogramsPerKilogram,
			  profile.TemperatureAnomalyCelsius,
			  profile.PressureAnomalyHectopascals,
			  profile.WindScaleMetresPerSecond.X,
			  profile.WindScaleMetresPerSecond.Y,
			  profile.WindScaleMetresPerSecond.Z,
			  profile.BaseTemperatureCelsius,
			  profile.BasePressureHectopascals,
			  profile.TemperatureLapseCelsiusPerKilometre})
			signature = scene::MixSignature(signature, std::bit_cast<uint32_t>(value));
		return signature;
	}

	uint64_t WeatherPhysicalSignature(const scene::Environment &environment, core::Name simulationLayer) {
		const auto &controls = environment.Weather;
		uint64_t signature = scene::MixSignature(environment.CloudProvider.Id, weather::Hash(controls.Seed));
		for (unsigned char byte : controls.LayerName.Text())
			signature = scene::MixSignature(signature, byte);
		for (unsigned char byte : simulationLayer.Text())
			signature = scene::MixSignature(signature, byte);
		for (uint64_t value :
			 {uint64_t(controls.AlgorithmVersion),
			  uint64_t(controls.DefinitionRevision),
			  uint64_t(controls.TickRate),
			  uint64_t(controls.FeatureCells),
			  environment.Clock.Epoch})
			signature = scene::MixSignature(signature, value);
		for (float value :
			 {controls.CellSizeMetres,
			  controls.BaseHeightMetres,
			  controls.ThicknessMetres,
			  controls.ExtentMetres.X,
			  controls.ExtentMetres.Y,
			  controls.ExtentMetres.Z,
			  controls.Coverage,
			  controls.CondensateScale,
			  controls.VapourMixingRatio,
			  controls.VapourScale,
			  controls.BaseTemperatureCelsius,
			  controls.BasePressureHectopascals,
			  controls.TemperatureLapseCelsiusPerKilometre,
			  controls.TemperatureVariationCelsius,
			  controls.PressureVariationHectopascals,
			  controls.WindMetresPerSecond.X,
			  controls.WindMetresPerSecond.Y,
			  controls.WindMetresPerSecond.Z,
			  controls.TurbulenceMetresPerSecond,
			  controls.ShearMetresPerSecond,
			  controls.LiftMetresPerSecond,
			  controls.FrontContrast,
			  controls.GrowthRate,
			  controls.PrecipitationRate,
			  controls.IceFraction})
			signature = scene::MixSignature(signature, std::bit_cast<uint32_t>(value));
		return signature;
	}

	bool RecordWeatherPages(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		SDL_GPUBuffer *pages,
		std::span<const glm::ivec4> keys
	) {
		ENGINE_PROFILE_CAT("weather residency upload", core::ProfileCategory::Render);
		if (!device || !command || !pages || keys.empty() || keys.size() > WEATHER_MAX_BRICKS) return false;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = static_cast<uint32_t>(keys.size_bytes());
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		if (!transfer) return false;
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		if (!mapped) {
			gpu::ReleaseTransferBuffer(device, transfer);
			return false;
		}
		std::memcpy(mapped, keys.data(), keys.size_bytes());
		SDL_UnmapGPUTransferBuffer(device, transfer);
		auto *pass = SDL_BeginGPUCopyPass(command);
		if (!pass) {
			gpu::ReleaseTransferBuffer(device, transfer);
			return false;
		}
		const SDL_GPUTransferBufferLocation source{transfer, 0};
		const SDL_GPUBufferRegion destination{pages, 0, info.size};
		SDL_UploadToGPUBuffer(pass, &source, &destination, true);
		SDL_EndGPUCopyPass(pass);
		gpu::ReleaseTransferBuffer(device, transfer);
		core::Metrics::Count("render.weather.page_upload_bytes", info.size);
		core::Metrics::Count("render.weather.page_uploads", 1);
		return true;
	}

	bool RecordWeatherEmptyField(SDL_GPUDevice *device, SDL_GPUCommandBuffer *command, SDL_GPUBuffer *cells) {
		ENGINE_PROFILE_CAT("weather empty binding initialization", core::ProfileCategory::Render);
		if (!device || !command || !cells) return false;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = 8;
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		if (!transfer) return false;
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		if (!mapped) {
			gpu::ReleaseTransferBuffer(device, transfer);
			return false;
		}
		const std::array<uint32_t, 2> neutral{0x7f007f00u, 0x007f7f7fu};
		std::memcpy(mapped, neutral.data(), sizeof(neutral));
		SDL_UnmapGPUTransferBuffer(device, transfer);
		auto *copy = SDL_BeginGPUCopyPass(command);
		if (!copy) {
			gpu::ReleaseTransferBuffer(device, transfer);
			return false;
		}
		const SDL_GPUTransferBufferLocation source{transfer, 0};
		const SDL_GPUBufferRegion destination{cells, 0, 8};
		SDL_UploadToGPUBuffer(copy, &source, &destination, true);
		SDL_EndGPUCopyPass(copy);
		gpu::ReleaseTransferBuffer(device, transfer);
		core::Metrics::Count("render.weather.empty_binding_initialized_bytes", 8);
		return true;
	}

	bool RecordWeatherField(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		const WeatherGpuUniforms &uniforms,
		uint64_t physicalBytes
	) {
		ENGINE_PROFILE_CAT("weather field generation", core::ProfileCategory::Render);
		if (!command || !pipeline || !cells || !pages || uniforms.Grid.w == 0) return false;
		SDL_GPUStorageBufferReadWriteBinding output{};
		output.buffer = cells;
		output.cycle = true;
		auto *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		SDL_BindGPUComputeStorageBuffers(pass, 0, &pages, 1);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Grid.w + 63) / 64, 1, 1);
		SDL_EndGPUComputePass(pass);
		core::Metrics::Count("render.weather.generated_cells", uniforms.Grid.w);
		core::Metrics::Count(
			"render.weather.generated_bytes",
			physicalBytes ? physicalBytes : uint64_t(uniforms.Grid.w) * weather::ENCODED_CELL_BYTES
		);
		core::Metrics::Count("render.weather.field_dispatches", 1);
		return true;
	}

	bool RecordWeatherSummary(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		SDL_GPUBuffer *summaries,
		const WeatherGpuUniforms &uniforms
	) {
		ENGINE_PROFILE_CAT("weather brick summary", core::ProfileCategory::Render);
		if (!command || !pipeline || !cells || !pages || !summaries || uniforms.Bricks.w == 0) return false;
		SDL_GPUStorageBufferReadWriteBinding output{};
		output.buffer = summaries;
		output.cycle = true;
		auto *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		const std::array<SDL_GPUBuffer *, 2> inputs{cells, pages};
		SDL_BindGPUComputeStorageBuffers(pass, 0, inputs.data(), inputs.size());
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Bricks.w + 63) / 64, 1, 1);
		SDL_EndGPUComputePass(pass);
		core::Metrics::Count("render.weather.summary_dispatches", 1);
		core::Metrics::Count("render.weather.resident_bricks", uniforms.Bricks.w);
		return true;
	}

	bool RecordWeatherRays(
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
	) {
		ENGINE_PROFILE_CAT("weather volume rays", core::ProfileCategory::Render);
		if (!command || !pipeline || !cells || !summaries || !pages || !previousPages || !source || !depth ||
			!destination || !cloudDepth || !sampler || !width || !height || source == destination ||
			depth == destination || source == cloudDepth || depth == cloudDepth || destination == cloudDepth)
			return false;
		std::array<SDL_GPUStorageTextureReadWriteBinding, 2> output{};
		output[0].texture = destination;
		output[1].texture = cloudDepth;
		for (auto &binding : output)
			binding.cycle = true;
		auto *pass = SDL_BeginGPUComputePass(command, output.data(), uint32_t(output.size()), nullptr, 0);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		const SDL_GPUTextureSamplerBinding images[2]{{source, sampler}, {depth, sampler}};
		SDL_BindGPUComputeSamplers(pass, 0, images, 2);
		SDL_GPUBuffer *buffers[4]{cells, summaries, pages, previousPages};
		SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, 4);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		core::Metrics::Count("render.weather.ray_dispatches", 1);
		core::Metrics::Count("render.weather.ray_pixels", uint64_t(width) * height);
		return true;
	}
}
