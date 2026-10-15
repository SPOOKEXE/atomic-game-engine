#include "WeatherGpuClock.hpp"

#include "GpuHeap.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <SDL3/SDL_gpu.h>

namespace engine::render {
	namespace {
		bool SameResidentTime(const WeatherGpuClockSample &left, const WeatherGpuClockSample &right) {
			return left.Time.PhaseTick == right.Time.PhaseTick &&
				   left.Time.DisplacementQ8 == right.Time.DisplacementQ8 &&
				   left.Time.Revision == right.Time.Revision;
		}

		uint64_t ClockSignature(const WeatherGpuClockSample &sample) {
			uint64_t signature = scene::MixSignature(sample.Identity, sample.Time.PhaseTick);
			for (uint32_t displacement : sample.Time.DisplacementQ8)
				signature = scene::MixSignature(signature, displacement);
			signature = scene::MixSignature(signature, sample.Time.Revision);
			return scene::MixSignature(signature, sample.TickRate);
		}
	}

	std::optional<WeatherGpuClockPlan> PlanWeatherGpuClock(
		const WeatherGpuClock &clock, const WeatherGpuClockSample &sample, const void *command
	) {
		if (!command || sample.TickRate == 0 || sample.TickRate > 120 || sample.Time.Revision == 0)
			return std::nullopt;
		if (clock.Publication.Pending && clock.Publication.PendingCommand != command) return std::nullopt;
		const bool pending = clock.Publication.Pending;
		const bool ready = pending || clock.Publication.Ready;
		const auto &previous = pending ? clock.PendingSample : clock.CommittedSample;
		const uint64_t anchorTick = pending ? clock.PendingAnchorTick : clock.AnchorTick;
		const bool sourceBackwards = ready && sample.SourceTick < previous.SourceTick;
		const bool phaseBackwards = ready && sample.Time.PhaseTick < previous.Time.PhaseTick;
		const bool phaseJump =
			ready && !phaseBackwards &&
			sample.Time.PhaseTick - previous.Time.PhaseTick > uint64_t(sample.TickRate) * 2;
		const bool anchorDue = ready && sample.SourceTick >= anchorTick &&
							   sample.SourceTick - anchorTick >= uint64_t(sample.TickRate) * 5;
		const bool anchor = !ready || sample.Identity != previous.Identity ||
							sample.Time.Revision != previous.Time.Revision ||
							sample.TickRate != previous.TickRate || sourceBackwards || phaseBackwards ||
							phaseJump || anchorDue;
		WeatherGpuClockPlan plan;
		plan.AnchorTick = anchor ? sample.SourceTick : anchorTick;
		if (!anchor && SameResidentTime(previous, sample)) return plan;
		plan.Action = anchor ? WeatherGpuClockAction::Anchor : WeatherGpuClockAction::Advance;
		const uint64_t phase =
			anchor ? sample.Time.PhaseTick : sample.Time.PhaseTick - previous.Time.PhaseTick;
		plan.Uniforms.Phase = {uint32_t(phase), uint32_t(phase >> 32), anchor ? 1u : 0u, 0};
		for (size_t axis = 0; axis < 3; ++axis)
			plan.Uniforms.Time[axis] =
				anchor ? sample.Time.DisplacementQ8[axis]
					   : sample.Time.DisplacementQ8[axis] - previous.Time.DisplacementQ8[axis];
		plan.Uniforms.Time[3] = sample.Time.Revision;
		return plan;
	}

	bool RecordWeatherGpuClock(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		WeatherGpuClock &clock,
		const WeatherGpuClockSample &sample
	) {
		ENGINE_PROFILE_CAT("weather GPU clock", core::ProfileCategory::Render);
		if (!device || !pipeline) return false;
		const auto plan = PlanWeatherGpuClock(clock, sample, command);
		if (!plan) return false;
		if (plan->Action == WeatherGpuClockAction::Reuse) {
			core::Metrics::Count("render.weather.clock_cache_hits", 1);
			return true;
		}
		if (!clock.Buffer) {
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
			info.size = sizeof(WeatherGpuClockUniforms);
			clock.Buffer = gpu::CreateBuffer(device, &info);
			if (!clock.Buffer) return false;
		}
		SDL_GPUStorageBufferReadWriteBinding output{};
		output.buffer = clock.Buffer;
		// Advances read their previous value, so rotating the allocation would lose the counter.
		output.cycle = false;
		auto *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		SDL_PushGPUComputeUniformData(command, 0, &plan->Uniforms, sizeof(plan->Uniforms));
		SDL_DispatchGPUCompute(pass, 1, 1, 1);
		SDL_EndGPUComputePass(pass);
		clock.PendingSample = sample;
		clock.PendingAnchorTick = plan->AnchorTick;
		clock.Publication.Stage(ClockSignature(sample), command);
		core::Metrics::Count("render.weather.clock_dispatches", 1);
		core::Metrics::Count("render.weather.clock_uniform_bytes", sizeof(plan->Uniforms));
		core::Metrics::Count(
			plan->Action == WeatherGpuClockAction::Anchor ? "render.weather.clock_anchors"
														  : "render.weather.clock_advances",
			1
		);
		return true;
	}

	void CommitWeatherGpuClock(WeatherGpuClock &clock, const void *command) {
		if (!clock.Publication.Pending || clock.Publication.PendingCommand != command) return;
		clock.CommittedSample = clock.PendingSample;
		clock.AnchorTick = clock.PendingAnchorTick;
		clock.Publication.Commit(command);
		clock.PendingSample = {};
		clock.PendingAnchorTick = 0;
	}

	void DiscardWeatherGpuClock(WeatherGpuClock &clock, const void *command) {
		if (!clock.Publication.Pending || (command && clock.Publication.PendingCommand != command)) return;
		// A partially submitted command may have advanced the in-place buffer already.
		clock.Publication.Ready = false;
		clock.Publication.Discard(command);
		clock.PendingSample = {};
		clock.PendingAnchorTick = 0;
	}

	void ReleaseWeatherGpuClock(SDL_GPUDevice *device, WeatherGpuClock &clock) {
		if (clock.Buffer) gpu::ReleaseBuffer(device, clock.Buffer);
		clock = {};
	}
}
