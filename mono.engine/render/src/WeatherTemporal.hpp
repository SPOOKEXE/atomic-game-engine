#pragma once

// Temporal storage belongs to one view. It contains cloud-only radiance so
// reconstructing a frame never accumulates the scene or applies opacity twice.

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

namespace engine::render {
	struct WeatherTemporalKey {
		uint64_t World = 0;
		uint64_t Provider = 0;
		uint64_t Content = 0;
		uint64_t Epoch = 0;
		uint64_t TimelineRevision = 0;
		uint64_t Tick = 0;
		uint32_t TickRate = 20;
		uint32_t Width = 0;
		uint32_t Height = 0;
		float CellSizeMetres = 64.0f;
		std::array<uint32_t, 3> DisplacementQ8{};
		bool CameraCut = false;
	};

	struct WeatherTemporalFrame {
		WeatherTemporalKey Key;
		glm::mat4 ViewProjection{1.0f};
		glm::vec3 Eye{};
	};

	struct WeatherTemporalHistory {
		SDL_GPUTexture *Clouds = nullptr;
		SDL_GPUTexture *Depth = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		WeatherTemporalFrame Accepted;
		WeatherTemporalFrame Pending;
		const void *PendingCommand = nullptr;
		bool Ready = false;

		void Stage(const WeatherTemporalFrame &frame, const void *command);
		void Commit(const void *command);
		void Discard(const void *command = nullptr);
	};

	// Both compute stages consume this exact private std140 layout.
	struct WeatherReconstructionUniforms {
		glm::mat4 InverseViewProjection{1.0f};
		glm::mat4 PreviousViewProjection{1.0f};
		glm::vec4 Eye{};
		glm::vec4 PreviousEye{};
		glm::vec4 CloudDisplacement{};
		glm::vec4 Limits{.85f, .02f, 2.0f, .1f};
		glm::uvec4 Extents{};
	};
	static_assert(sizeof(WeatherReconstructionUniforms) == 208);

	[[nodiscard]] bool
	WeatherTemporalCompatible(const WeatherTemporalFrame &previous, const WeatherTemporalFrame &current);
	[[nodiscard]] glm::vec3
	WeatherCloudDisplacement(const WeatherTemporalKey &previous, const WeatherTemporalKey &current);
	[[nodiscard]] bool WeatherDepthCompatible(float firstDistance, float secondDistance);
	[[nodiscard]] glm::vec4 WeatherCloudComposite(const glm::vec4 &scene, const glm::vec4 &cloud);
	[[nodiscard]] bool EnsureWeatherTemporalHistory(
		SDL_GPUDevice *device, WeatherTemporalHistory &history, uint32_t width, uint32_t height
	);
	void ReleaseWeatherTemporalHistory(SDL_GPUDevice *device, WeatherTemporalHistory &history);
	[[nodiscard]] bool RecordWeatherReconstruction(
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
		bool retainHistory = true
	);
}
