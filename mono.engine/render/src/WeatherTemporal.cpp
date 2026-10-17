#include "WeatherTemporal.hpp"

#include "GpuHeap.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::render {
	void WeatherTemporalHistory::Stage(const WeatherTemporalFrame &frame, const void *command) {
		Pending = frame;
		PendingCommand = command;
	}

	void WeatherTemporalHistory::Commit(const void *command) {
		if (!command || PendingCommand != command) return;
		Accepted = Pending;
		Ready = true;
		PendingCommand = nullptr;
	}

	void WeatherTemporalHistory::Discard(const void *command) {
		if (command && PendingCommand != command) return;
		// SDL may already have cycled a destination while recording the copy.
		// A cancelled command cannot certify that new backing as initialized.
		if (PendingCommand) Ready = false;
		PendingCommand = nullptr;
	}

	glm::vec3
	WeatherCloudDisplacement(const WeatherTemporalKey &previous, const WeatherTemporalKey &current) {
		glm::vec3 displacement;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint32_t delta = current.DisplacementQ8[axis] - previous.DisplacementQ8[axis];
			displacement[axis] = float(std::bit_cast<int32_t>(delta)) * (current.CellSizeMetres / 256.0f);
		}
		return displacement;
	}

	bool
	WeatherTemporalCompatible(const WeatherTemporalFrame &previous, const WeatherTemporalFrame &current) {
		const auto &first = previous.Key;
		const auto &second = current.Key;
		if (second.CameraCut || first.World != second.World || first.Provider != second.Provider ||
			first.Content != second.Content || first.Epoch != second.Epoch ||
			first.TimelineRevision != second.TimelineRevision || first.Width != second.Width ||
			first.Height != second.Height || first.CellSizeMetres != second.CellSizeMetres ||
			first.TickRate != second.TickRate || second.Tick < first.Tick ||
			second.Tick - first.Tick > std::max(1u, second.TickRate / 2u))
			return false;
		if (!second.Width || !second.Height || !std::isfinite(second.CellSizeMetres) ||
			!(second.CellSizeMetres > 0))
			return false;
		const float cameraTravel = glm::length(current.Eye - previous.Eye);
		const float cloudTravel = glm::length(WeatherCloudDisplacement(first, second));
		if (!std::isfinite(cameraTravel) || !std::isfinite(cloudTravel) ||
			cameraTravel > std::max(32.0f, second.CellSizeMetres * 2) ||
			cloudTravel > second.CellSizeMetres * 2)
			return false;
		for (uint32_t column = 0; column < 4; ++column)
			for (uint32_t row = 0; row < 4; ++row)
				if (!std::isfinite(current.ViewProjection[column][row]) ||
					!std::isfinite(previous.ViewProjection[column][row]))
					return false;
		return true;
	}

	bool WeatherDepthCompatible(float firstDistance, float secondDistance) {
		if (!std::isfinite(firstDistance) || !std::isfinite(secondDistance) || firstDistance < 0 ||
			secondDistance < 0)
			return false;
		return std::abs(firstDistance - secondDistance) <= std::max(2.0f, firstDistance * .02f);
	}

	glm::vec4 WeatherCloudComposite(const glm::vec4 &scene, const glm::vec4 &cloud) {
		const float transmittance = std::clamp(cloud.w, 0.0f, 1.0f);
		return {glm::vec3(cloud) + glm::vec3(scene) * transmittance, 1.0f - (1.0f - scene.w) * transmittance};
	}

	bool EnsureWeatherTemporalHistory(
		SDL_GPUDevice *device, WeatherTemporalHistory &history, uint32_t width, uint32_t height
	) {
		if (!device || !width || !height) return false;
		if (history.Clouds && history.Depth && history.Width == width && history.Height == height)
			return true;
		ReleaseWeatherTemporalHistory(device, history);
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		history.Clouds = gpu::CreateTexture(device, &info);
		history.Depth = gpu::CreateTexture(device, &info);
		if (!history.Clouds || !history.Depth) {
			ReleaseWeatherTemporalHistory(device, history);
			return false;
		}
		history.Width = width;
		history.Height = height;
		return true;
	}

	void ReleaseWeatherTemporalHistory(SDL_GPUDevice *device, WeatherTemporalHistory &history) {
		if (history.Clouds) gpu::ReleaseTexture(device, history.Clouds);
		if (history.Depth) gpu::ReleaseTexture(device, history.Depth);
		history = {};
	}

	bool RecordWeatherReconstruction(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *temporalPipeline,
		SDL_GPUComputePipeline *reconstructPipeline,
		SDL_GPUTexture *source,
		SDL_GPUTexture *depth,
		SDL_GPUTexture *clouds,
		SDL_GPUTexture *cloudDepth,
		SDL_GPUTexture *resolved,
		SDL_GPUTexture *destination,
		WeatherTemporalHistory &history,
		SDL_GPUSampler *sampler,
		const WeatherReconstructionUniforms &uniforms,
		bool retainHistory
	) {
		ENGINE_PROFILE_CAT("weather temporal reconstruction", core::ProfileCategory::Render);
		if (!command || !temporalPipeline || !reconstructPipeline || !source || !depth || !clouds ||
			!cloudDepth || !resolved || !destination ||
			(retainHistory && (!history.Clouds || !history.Depth)) || !sampler || !uniforms.Extents.x ||
			!uniforms.Extents.y || !uniforms.Extents.z || !uniforms.Extents.w)
			return false;
		for (auto *input : {source, depth, clouds, cloudDepth, history.Clouds, history.Depth})
			if (input == resolved || input == destination) return false;
		if (resolved == destination) return false;
		SDL_GPUStorageTextureReadWriteBinding halfOutput{};
		halfOutput.texture = resolved;
		halfOutput.cycle = true;
		auto *pass = SDL_BeginGPUComputePass(command, &halfOutput, 1, nullptr, 0);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, temporalPipeline);
		// Invalid history binds today's initialized samples and sets weight zero.
		// Even speculative shader reads can never observe an unwritten allocation.
		const bool historyReadable = retainHistory && uniforms.Limits.x > 0 && history.Ready;
		const std::array temporalInputs{
			SDL_GPUTextureSamplerBinding{clouds, sampler},
			SDL_GPUTextureSamplerBinding{cloudDepth, sampler},
			SDL_GPUTextureSamplerBinding{historyReadable ? history.Clouds : clouds, sampler},
			SDL_GPUTextureSamplerBinding{historyReadable ? history.Depth : cloudDepth, sampler}
		};
		SDL_BindGPUComputeSamplers(pass, 0, temporalInputs.data(), uint32_t(temporalInputs.size()));
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Extents.z + 7) / 8, (uniforms.Extents.w + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		SDL_GPUStorageTextureReadWriteBinding fullOutput{};
		fullOutput.texture = destination;
		fullOutput.cycle = true;
		pass = SDL_BeginGPUComputePass(command, &fullOutput, 1, nullptr, 0);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, reconstructPipeline);
		const std::array reconstructInputs{
			SDL_GPUTextureSamplerBinding{source, sampler},
			SDL_GPUTextureSamplerBinding{depth, sampler},
			SDL_GPUTextureSamplerBinding{resolved, sampler},
			SDL_GPUTextureSamplerBinding{cloudDepth, sampler}
		};
		SDL_BindGPUComputeSamplers(pass, 0, reconstructInputs.data(), uint32_t(reconstructInputs.size()));
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Extents.x + 7) / 8, (uniforms.Extents.y + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		if (retainHistory)
			for (const auto images :
				 {std::array{resolved, history.Clouds}, std::array{cloudDepth, history.Depth}}) {
				SDL_GPUBlitInfo copy{};
				copy.source = {images[0], 0, 0, 0, 0, uniforms.Extents.z, uniforms.Extents.w};
				copy.destination = {images[1], 0, 0, 0, 0, uniforms.Extents.z, uniforms.Extents.w};
				copy.load_op = SDL_GPU_LOADOP_DONT_CARE;
				copy.filter = SDL_GPU_FILTER_NEAREST;
				copy.cycle = true;
				SDL_BlitGPUTexture(command, &copy);
			}
		core::Metrics::Count("render.weather.temporal_dispatches", 1);
		core::Metrics::Count("render.weather.reconstruct_dispatches", 1);
		core::Metrics::Count(
			"render.weather.history_copy_bytes",
			retainHistory ? uint64_t(history.Width) * history.Height * 16 : 0
		);
		core::Metrics::Count("render.weather.history_samples", historyReadable ? 1 : 0);
		return true;
	}
}
