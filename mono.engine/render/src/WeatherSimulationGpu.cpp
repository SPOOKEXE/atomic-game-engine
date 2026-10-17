#include "WeatherSimulationGpu.hpp"

#include "GpuHeap.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/weather/Simulation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace engine::render {
	namespace {
		uint64_t TerrainSignature(const weather::SimulationState &state) {
			uint64_t signature = 14695981039346656037ull;
			for (float height : state.GroundHeightMetres) {
				const uint32_t word = std::bit_cast<uint32_t>(height);
				for (uint32_t shift = 0; shift < 32; shift += 8)
					signature = (signature ^ ((word >> shift) & 255)) * 1099511628211ull;
			}
			for (uint8_t mask : state.SolidMask)
				signature = (signature ^ mask) * 1099511628211ull;
			return signature;
		}

		SDL_GPUBuffer *Buffer(SDL_GPUDevice *device, uint32_t bytes) {
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
			info.size = bytes;
			auto *buffer = gpu::CreateBuffer(device, &info);
			if (buffer) core::Metrics::Count("render.weather.stateful.allocated_bytes", bytes);
			return buffer;
		}

		bool Allocate(SDL_GPUDevice *device, WeatherSimulationGpu &state, uint32_t count) {
			state.CellCount = count;
			state.PublishedCells = Buffer(device, count * 32);
			for (auto &buffer : state.WorkingCells)
				buffer = Buffer(device, count * 32);
			state.ForcedCells = Buffer(device, count * 32);
			state.ProjectedCells = Buffer(device, count * 32);
			for (auto &buffer : state.Scratch)
				buffer = Buffer(device, count * 16);
			state.Diagnostics = Buffer(device, count * 32);
			state.PublishedPacked = Buffer(device, count * 8);
			state.StagingPacked = Buffer(device, count * 8);
			state.PublishedTerrain = Buffer(device, count * 4);
			state.StagingTerrain = Buffer(device, count * 4);
			state.PublishedRain = Buffer(device, count * 4);
			for (auto &buffer : state.WorkingRain)
				buffer = Buffer(device, count * 4);
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = ((count + 63) / 64) * 32;
			state.Readback = gpu::CreateTransferBuffer(device, &info);
			if (state.Readback) core::Metrics::Count("render.weather.stateful.allocated_bytes", info.size);
			return state.PublishedCells && state.WorkingCells[0] && state.WorkingCells[1] &&
				   state.ForcedCells && state.ProjectedCells && state.Scratch[0] && state.Scratch[1] &&
				   state.Diagnostics && state.PublishedPacked && state.StagingPacked && state.Readback &&
				   state.PublishedTerrain && state.StagingTerrain && state.PublishedRain &&
				   state.WorkingRain[0] && state.WorkingRain[1];
		}

		bool Upload(
			SDL_GPUDevice *device,
			SDL_GPUCommandBuffer *command,
			SDL_GPUBuffer *buffer,
			std::span<const std::byte> bytes
		) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = static_cast<uint32_t>(bytes.size());
			auto *transfer = gpu::CreateTransferBuffer(device, &info);
			if (!transfer) return false;
			void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
			if (!mapped) {
				gpu::ReleaseTransferBuffer(device, transfer);
				return false;
			}
			std::memcpy(mapped, bytes.data(), bytes.size());
			SDL_UnmapGPUTransferBuffer(device, transfer);
			auto *pass = SDL_BeginGPUCopyPass(command);
			if (!pass) {
				gpu::ReleaseTransferBuffer(device, transfer);
				return false;
			}
			const SDL_GPUTransferBufferLocation source{transfer, 0};
			const SDL_GPUBufferRegion destination{buffer, 0, info.size};
			SDL_UploadToGPUBuffer(pass, &source, &destination, false);
			SDL_EndGPUCopyPass(pass);
			gpu::ReleaseTransferBuffer(device, transfer);
			core::Metrics::Count("render.weather.stateful.upload_bytes", info.size);
			return true;
		}

		bool Dispatch(
			SDL_GPUCommandBuffer *command,
			SDL_GPUComputePipeline *pipeline,
			WeatherSimulationGpu &state,
			const SimulationGpuUniforms &uniforms,
			SDL_GPUBuffer *inputCells,
			SDL_GPUBuffer *projectedCells,
			SDL_GPUBuffer *inputScratch,
			SDL_GPUBuffer *outputCells,
			SDL_GPUBuffer *outputScratch,
			SDL_GPUBuffer *terrain,
			SDL_GPUBuffer *inputRain,
			SDL_GPUBuffer *outputRain
		) {
			SDL_GPUStorageBufferReadWriteBinding outputs[5]{};
			SDL_GPUBuffer *outputBuffers[5]{
				outputCells, outputScratch, state.StagingPacked, state.Diagnostics, outputRain
			};
			for (size_t index = 0; index < 5; ++index) {
				outputs[index].buffer = outputBuffers[index];
				outputs[index].cycle = false;
			}
			auto *pass = SDL_BeginGPUComputePass(command, nullptr, 0, outputs, 5);
			if (!pass) return false;
			SDL_BindGPUComputePipeline(pass, pipeline);
			SDL_GPUBuffer *inputs[5]{inputCells, projectedCells, inputScratch, terrain, inputRain};
			SDL_BindGPUComputeStorageBuffers(pass, 0, inputs, 5);
			SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
			const uint32_t workItems = uniforms.Grid[3] == 7 ? (state.CellCount + 63) / 64 : state.CellCount;
			const uint32_t groups = (workItems + 63) / 64;
			SDL_DispatchGPUCompute(pass, groups, 1, 1);
			++state.RecordedDispatches;
			SDL_EndGPUComputePass(pass);
			core::Metrics::Count("render.weather.stateful.dispatches", 1);
			core::Metrics::Count("render.weather.stateful.workgroups", groups);
			return true;
		}

		bool ValidProfile(const weather::EncodingProfile &profile) {
			const float positive[]{
				profile.CellSizeMetres,
				profile.CondensateKilogramsPerCubicMetre,
				profile.VapourKilogramsPerKilogram,
				profile.TemperatureAnomalyCelsius,
				profile.PressureAnomalyHectopascals,
				profile.WindScaleMetresPerSecond.X,
				profile.WindScaleMetresPerSecond.Y,
				profile.WindScaleMetresPerSecond.Z
			};
			for (float scalar : positive)
				if (!std::isfinite(scalar) || scalar < 0.000001f || scalar > 4096) return false;
			return std::isfinite(profile.BaseTemperatureCelsius) && profile.BaseTemperatureCelsius >= -100 &&
				   profile.BaseTemperatureCelsius <= 100 && std::isfinite(profile.BasePressureHectopascals) &&
				   profile.BasePressureHectopascals >= 10 && profile.BasePressureHectopascals <= 2000 &&
				   std::isfinite(profile.TemperatureLapseCelsiusPerKilometre) &&
				   profile.TemperatureLapseCelsiusPerKilometre >= 0 &&
				   profile.TemperatureLapseCelsiusPerKilometre <= 20;
		}

		void CollectTimings(WeatherSimulationGpu &state) {
			if (!state.Timings) return;
			for (uint32_t slot = 0; slot < VulkanTimestamps::SLOTS; ++slot) {
				double times[VulkanTimestamps::MARKS]{};
				uint32_t count = 0;
				if (!state.Timings->Collect(slot, times, count) || count < 2) continue;
				const float milliseconds =
					static_cast<float>(VulkanTimestamps::Between(times, 0, 1) / 1000000.0);
				core::FrameGraph::Report(
					"gpu weather stateful transaction", core::ProfileCategory::Gpu, milliseconds
				);
				core::Metrics::SetGauge("render.weather.stateful.gpu_milliseconds", milliseconds);
			}
		}
	}

	bool StageWeatherSimulationGpu(
		SDL_GPUDevice *device,
		SDL_GPUComputePipeline *pipeline,
		WeatherSimulationGpu &state,
		uint64_t targetTick,
		const weather::EncodingProfile &profile,
		std::span<const std::byte> checkpoint,
		uint32_t maximumReplayTicks,
		const WeatherSimulationRecordHook &recordHook,
		const weather::SimulationDefinition *expectedDefinition,
		bool boundReplayTarget
	) {
		ENGINE_PROFILE_CAT("weather resident evolution", core::ProfileCategory::Render);
		state.LastSubmittedDispatches = 0;
		state.RecordedDispatches = 0;
		if (!device || !pipeline || state.Fence || !ValidProfile(profile)) return false;
		if (checkpoint.empty() && expectedDefinition && state.Definition != *expectedDefinition) return false;
		CollectTimings(state);
		const std::array<float, 11> encoding{
			profile.CellSizeMetres,
			profile.CondensateKilogramsPerCubicMetre,
			profile.VapourKilogramsPerKilogram,
			profile.TemperatureAnomalyCelsius,
			profile.PressureAnomalyHectopascals,
			profile.WindScaleMetresPerSecond.X,
			profile.WindScaleMetresPerSecond.Y,
			profile.WindScaleMetresPerSecond.Z,
			profile.BaseTemperatureCelsius,
			profile.BasePressureHectopascals,
			profile.TemperatureLapseCelsiusPerKilometre
		};
		if (state.Published && checkpoint.empty() && targetTick == state.Tick &&
			encoding == state.PublishedEncoding)
			return true;
		weather::SimulationState decoded;
		uint64_t tick = state.Tick;
		uint64_t terrainSignature = state.TerrainSignature;
		auto definition = state.Definition;
		auto history = state.ForcingHistory;
		if (!checkpoint.empty()) {
			if (!weather::DecodeCheckpoint(checkpoint, decoded)) return false;
			terrainSignature = TerrainSignature(decoded);
			if (state.Published && decoded.Definition.TerrainRevision == state.Definition.TerrainRevision &&
				terrainSignature != state.TerrainSignature)
				return false;
			definition = decoded.Definition;
			history = decoded.ForcingHistory;
			tick = decoded.Tick;
			if (state.CellCount && definition.Grid != state.Definition.Grid) return false;
		} else if (!state.Published)
			return false;
		if (expectedDefinition && definition != *expectedDefinition) return false;
		if (boundReplayTarget)
			targetTick =
				std::min(targetTick, tick + std::min<uint64_t>(maximumReplayTicks, UINT64_MAX - tick));
		if (targetTick < tick || targetTick - tick > maximumReplayTicks || maximumReplayTicks > 128 ||
			definition.MaximumStateBytes <
				uint64_t(state.CellCount ? state.CellCount : decoded.Cells.size()) * 260 +
					((uint64_t(state.CellCount ? state.CellCount : decoded.Cells.size()) + 63) / 64) * 32)
			return false;
		if (!state.CellCount && !Allocate(device, state, static_cast<uint32_t>(decoded.Cells.size()))) {
			ReleaseWeatherSimulationGpu(device, state);
			return false;
		}
		if (!state.Published) state.Definition = definition;
		if (!state.TimingAttempted) {
			state.TimingAttempted = true;
			state.Timings = std::make_unique<VulkanTimestamps>();
			(void)state.Timings->Probe(device);
			core::Metrics::SetGauge(
				"render.weather.stateful.gpu_timing_available", state.Timings->Ready() ? 1 : 0
			);
		}
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) return false;
		const auto cancel = [&] {
			if (state.Timings) state.Timings->Abandon(state.TimingSlot);
			if (recordHook) recordHook(command, WeatherSimulationRecordEvent::Cancel);
			SDL_CancelGPUCommandBuffer(command);
			return false;
		};
		state.TimingSlot = state.Timings ? state.Timings->Begin(command) : VulkanTimestamps::NO_SLOT;
		if (state.TimingSlot != VulkanTimestamps::NO_SLOT) (void)state.Timings->Mark(command);
		if (recordHook) recordHook(command, WeatherSimulationRecordEvent::Begin);
		uint32_t currentIndex = 0;
		SDL_GPUBuffer *currentCells = state.PublishedCells;
		SDL_GPUBuffer *currentRain = state.PublishedRain;
		SDL_GPUBuffer *terrain = checkpoint.empty() ? state.PublishedTerrain : state.StagingTerrain;
		if (!checkpoint.empty()) {
			std::vector<uint32_t> flags(state.CellCount);
			for (uint32_t index = 0; index < state.CellCount; ++index)
				flags[index] = weather::SimulationCellFlags(decoded, index);
			if (!Upload(device, command, state.WorkingCells[0], std::as_bytes(std::span(decoded.Cells))) ||
				!Upload(
					device,
					command,
					state.WorkingRain[0],
					std::as_bytes(std::span(decoded.FallingCondensateKilogramsPerCubicMetre))
				) ||
				!Upload(device, command, terrain, std::as_bytes(std::span(flags))))
				return cancel();
			currentCells = state.WorkingCells[0];
			currentRain = state.WorkingRain[0];
		}
		const auto dispatch = [&](const SimulationGpuUniforms &uniforms,
								  SDL_GPUBuffer *cells,
								  SDL_GPUBuffer *projected,
								  SDL_GPUBuffer *scratch,
								  SDL_GPUBuffer *nextCells,
								  SDL_GPUBuffer *nextScratch) {
			return Dispatch(
				command,
				pipeline,
				state,
				uniforms,
				cells,
				projected,
				scratch,
				nextCells,
				nextScratch,
				terrain,
				currentRain,
				state.WorkingRain[currentRain == state.WorkingRain[0] ? 1 : 0]
			);
		};
		const auto forcingAt = [&](uint64_t at) -> const weather::SimulationForcing & {
			const auto found = std::upper_bound(
				history.begin(), history.end(), at, [](uint64_t wanted, const auto &forcing) {
					return wanted < forcing.Tick;
				}
			);
			return *std::prev(found);
		};
		if (!dispatch(
				SimulationUniforms(definition, forcingAt(tick), 6, 1, profile),
				currentCells,
				state.PublishedCells,
				state.Scratch[0],
				state.ForcedCells,
				state.Scratch[1]
			))
			return cancel();
		while (tick < targetTick) {
			const auto &forcing = forcingAt(tick);
			const auto substeps = weather::SimulationSubsteps(definition, forcing);
			if (!substeps) return cancel();
			const float seconds = 1.0f / (static_cast<float>(definition.TickRate) * *substeps);
			for (uint32_t substep = 0; substep < *substeps; ++substep) {
				const auto uniforms = [&](uint32_t phase) {
					return SimulationUniforms(definition, forcing, phase, seconds, profile);
				};
				if (!dispatch(
						uniforms(0),
						currentCells,
						state.PublishedCells,
						state.Scratch[0],
						state.ForcedCells,
						state.Scratch[1]
					))
					return cancel();
				if (!dispatch(
						uniforms(1),
						state.ForcedCells,
						state.PublishedCells,
						state.Scratch[1],
						state.WorkingCells[currentIndex ^ 1],
						state.Scratch[0]
					))
					return cancel();
				uint32_t scratchIndex = 0;
				for (uint32_t iteration = 0; iteration < definition.ProjectionIterations; ++iteration) {
					if (!dispatch(
							uniforms(2),
							state.ForcedCells,
							state.PublishedCells,
							state.Scratch[scratchIndex],
							state.WorkingCells[currentIndex ^ 1],
							state.Scratch[scratchIndex ^ 1]
						))
						return cancel();
					scratchIndex ^= 1;
				}
				if (!dispatch(
						uniforms(3),
						state.ForcedCells,
						state.PublishedCells,
						state.Scratch[scratchIndex],
						state.ProjectedCells,
						state.Scratch[scratchIndex ^ 1]
					))
					return cancel();
				const uint32_t nextIndex = currentCells == state.WorkingCells[0] ? 1 : 0;
				if (!dispatch(
						uniforms(4),
						currentCells,
						state.ProjectedCells,
						state.Scratch[scratchIndex],
						state.WorkingCells[nextIndex],
						state.Scratch[scratchIndex ^ 1]
					))
					return cancel();
				currentIndex = nextIndex;
				currentCells = state.WorkingCells[currentIndex];
				currentRain = state.WorkingRain[currentIndex];
			}
			++tick;
		}
		if (!dispatch(
				SimulationUniforms(definition, forcingAt(tick), 5, 1, profile),
				currentCells,
				state.PublishedCells,
				state.Scratch[0],
				state.ForcedCells,
				state.Scratch[1]
			))
			return cancel();
		if (!dispatch(
				SimulationUniforms(definition, forcingAt(tick), 7, 1, profile),
				currentCells,
				state.PublishedCells,
				state.Scratch[0],
				state.ForcedCells,
				state.Scratch[1]
			))
			return cancel();
		auto *pass = SDL_BeginGPUCopyPass(command);
		if (!pass) return cancel();
		const uint32_t diagnosticBytes = ((state.CellCount + 63) / 64) * 32;
		const SDL_GPUBufferRegion source{state.Scratch[1], 0, diagnosticBytes};
		const SDL_GPUTransferBufferLocation destination{state.Readback, 0};
		SDL_DownloadFromGPUBuffer(pass, &source, &destination);
		SDL_EndGPUCopyPass(pass);
		if (recordHook) recordHook(command, WeatherSimulationRecordEvent::End);
		if (state.TimingSlot != VulkanTimestamps::NO_SLOT) (void)state.Timings->Mark(command);
		state.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (!state.Fence) {
			if (state.Timings) state.Timings->Abandon(state.TimingSlot);
			if (recordHook) recordHook(command, WeatherSimulationRecordEvent::Cancel);
			return false;
		}
		if (state.Timings) state.Timings->Submitted(state.TimingSlot);
		state.PendingTick = targetTick;
		state.PendingCellIndex = currentCells == state.PublishedCells ? 2 : currentIndex;
		state.PendingRainIndex = currentRain == state.PublishedRain ? 2 : currentIndex;
		state.PendingTerrain = !checkpoint.empty();
		state.PendingTerrainSignature = terrainSignature;
		state.PendingDefinition = std::move(definition);
		state.PendingForcingHistory = std::move(history);
		state.PendingEncoding = encoding;
		state.LastSubmittedDispatches = state.RecordedDispatches;
		core::Metrics::Count("render.weather.stateful.readback_bytes", diagnosticBytes);
		core::Metrics::Count("render.weather.stateful.queue_submissions", 1);
		return true;
	}

	bool PollWeatherSimulationGpu(SDL_GPUDevice *device, WeatherSimulationGpu &state) {
		CollectTimings(state);
		if (!device || !state.Fence || !SDL_QueryGPUFence(device, state.Fence)) return false;
		SDL_ReleaseGPUFence(device, state.Fence);
		state.Fence = nullptr;
		const auto *diagnostics =
			static_cast<const float *>(SDL_MapGPUTransferBuffer(device, state.Readback, false));
		if (!diagnostics) return false;
		bool valid = true;
		for (uint32_t index = 0; index < (state.CellCount + 63) / 64; ++index) {
			const float *row = diagnostics + index * 8;
			for (size_t lane = 0; lane < 8; ++lane)
				valid &= std::isfinite(row[lane]);
			valid &= row[6] == 1;
		}
		SDL_UnmapGPUTransferBuffer(device, state.Readback);
		if (!valid) {
			core::Metrics::Count("render.weather.stateful.refused", 1);
			return false;
		}
		if (state.PendingCellIndex < 2)
			std::swap(state.PublishedCells, state.WorkingCells[state.PendingCellIndex]);
		if (state.PendingRainIndex < 2)
			std::swap(state.PublishedRain, state.WorkingRain[state.PendingRainIndex]);
		if (state.PendingTerrain) std::swap(state.PublishedTerrain, state.StagingTerrain);
		state.TerrainSignature = state.PendingTerrainSignature;
		std::swap(state.PublishedPacked, state.StagingPacked);
		state.Definition = std::move(state.PendingDefinition);
		state.ForcingHistory = std::move(state.PendingForcingHistory);
		state.Tick = state.PendingTick;
		state.Published = true;
		state.PublishedEncoding = state.PendingEncoding;
		core::Metrics::Count("render.weather.stateful.published", 1);
		return true;
	}

	void ReleaseWeatherSimulationGpu(SDL_GPUDevice *device, WeatherSimulationGpu &state) {
		if (!device) return;
		if (state.Fence && state.Timings && state.Timings->Ready() &&
			!SDL_QueryGPUFence(device, state.Fence)) {
			// Native query pools cannot use SDL's deferred buffer destruction. Retirement waits only here.
			ENGINE_PROFILE_CAT("weather timestamp retirement wait", core::ProfileCategory::Idle);
			(void)SDL_WaitForGPUFences(device, true, &state.Fence, 1);
		}
		if (state.Fence) SDL_ReleaseGPUFence(device, state.Fence);
		gpu::ReleaseBuffer(device, state.PublishedCells);
		for (auto *buffer : state.WorkingCells)
			gpu::ReleaseBuffer(device, buffer);
		gpu::ReleaseBuffer(device, state.ForcedCells);
		gpu::ReleaseBuffer(device, state.ProjectedCells);
		for (auto *buffer : state.Scratch)
			gpu::ReleaseBuffer(device, buffer);
		gpu::ReleaseBuffer(device, state.Diagnostics);
		gpu::ReleaseBuffer(device, state.PublishedPacked);
		gpu::ReleaseBuffer(device, state.StagingPacked);
		gpu::ReleaseBuffer(device, state.PublishedTerrain);
		gpu::ReleaseBuffer(device, state.StagingTerrain);
		gpu::ReleaseBuffer(device, state.PublishedRain);
		for (auto *buffer : state.WorkingRain)
			gpu::ReleaseBuffer(device, buffer);
		gpu::ReleaseTransferBuffer(device, state.Readback);
		state = {};
	}

	SimulationGpuUniforms SimulationUniforms(
		const weather::SimulationDefinition &definition,
		const weather::SimulationForcing &forcing,
		uint32_t phase,
		float seconds,
		const weather::EncodingProfile &profile
	) {
		SimulationGpuUniforms uniforms;
		uniforms.Grid = {definition.Grid.Cells[0], definition.Grid.Cells[1], definition.Grid.Cells[2], phase};
		uniforms.Boundaries = {
			static_cast<uint32_t>(definition.Grid.Boundaries[0]),
			static_cast<uint32_t>(definition.Grid.Boundaries[1]),
			static_cast<uint32_t>(definition.Grid.Boundaries[2]),
			definition.Grid.Cells[0] * definition.Grid.Cells[1] * definition.Grid.Cells[2]
		};
		uniforms.Integration = {
			seconds,
			definition.Grid.CellSizeMetres,
			definition.DryAirKilogramsPerCubicMetre,
			definition.DiffusionSquareMetresPerSecond
		};
		uniforms.Phase = {
			definition.ReferenceTemperatureCelsius,
			definition.SaturationVapourRatio,
			definition.SaturationSlopePerCelsius,
			definition.PhaseRelaxationPerSecond
		};
		uniforms.Thermal = {
			definition.LatentHeatJoulesPerKilogram,
			definition.HeatCapacityJoulesPerKilogramCelsius,
			definition.BuoyancyMetresPerSecondSquaredPerCelsius,
			definition.MaximumVelocityMetresPerSecond
		};
		uniforms.Precipitation = {
			definition.PrecipitationThresholdKilogramsPerCubicMetre,
			definition.PrecipitationPerSecond,
			definition.MaximumDivergencePerSecond,
			definition.RainFallSpeedMetresPerSecond
		};
		uniforms.Acceleration = {
			forcing.AccelerationMetresPerSecondSquared[0],
			forcing.AccelerationMetresPerSecondSquared[1],
			forcing.AccelerationMetresPerSecondSquared[2],
			forcing.CoolingCelsiusPerSecond
		};
		uniforms.WindRelaxation = {
			forcing.WindTargetMetresPerSecond[0],
			forcing.WindTargetMetresPerSecond[1],
			forcing.WindTargetMetresPerSecond[2],
			forcing.WindRelaxationPerSecond
		};
		uniforms.Sources = {
			forcing.VapourSourcePerSecond,
			forcing.CondensateSourceKilogramsPerCubicMetrePerSecond,
			definition.GroundVapourFluxKilogramsPerSquareMetreSecond,
			0
		};
		uniforms.OpenVelocity = {
			forcing.OpenVelocityMetresPerSecond[0],
			forcing.OpenVelocityMetresPerSecond[1],
			forcing.OpenVelocityMetresPerSecond[2],
			0
		};
		uniforms.InflowScalars = {
			forcing.Inflow.VapourKilogramsPerKilogram,
			forcing.Inflow.CondensateKilogramsPerCubicMetre,
			forcing.Inflow.TemperatureCelsius,
			forcing.Inflow.AtmosphericPressurePascals
		};
		uniforms.InflowVelocity = {
			forcing.Inflow.VelocityMetresPerSecond[0],
			forcing.Inflow.VelocityMetresPerSecond[1],
			forcing.Inflow.VelocityMetresPerSecond[2],
			forcing.Inflow.ProjectionPressurePascals
		};
		uniforms.EncodingScalar = {
			profile.CondensateKilogramsPerCubicMetre,
			profile.VapourKilogramsPerKilogram,
			profile.TemperatureAnomalyCelsius,
			profile.PressureAnomalyHectopascals
		};
		uniforms.EncodingBackground = {
			profile.BaseTemperatureCelsius,
			profile.BasePressureHectopascals,
			profile.TemperatureLapseCelsiusPerKilometre,
			profile.CellSizeMetres
		};
		uniforms.EncodingWind = {
			profile.WindScaleMetresPerSecond.X,
			profile.WindScaleMetresPerSecond.Y,
			profile.WindScaleMetresPerSecond.Z,
			0
		};
		uniforms.Origin = {
			definition.Grid.Origin[0], definition.Grid.Origin[1], definition.Grid.Origin[2], 0
		};
		return uniforms;
	}
}
