#pragma once

// Light-ray cloud optical depth, retained independently of visible cloud rays.
// The graph owns the texture; these records only describe its accepted contents.

#include "GraphHistory.hpp"
#include "WeatherResidency.hpp"

#include <engine/core/types/AABB.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/vec4.hpp>

#include <array>
#include <optional>

namespace engine::render {

	struct WeatherGpuInputs;

	inline constexpr uint32_t WEATHER_SHADOW_CASCADES = 4;

	// Fragment uniform slot three. Regions are cloud-bottom X/Z centres, radii and half-texel UV padding.
	struct WeatherShadowLightingUniforms {
		std::array<glm::vec4, WEATHER_SHADOW_CASCADES> Regions{};
		// strength, cloud bottom, cloud top, cascade count.
		glm::vec4 Controls{};
		// Unit direction toward the sun; w is the outer receiver radius.
		glm::vec4 ToSun{};
		// Receiver-domain X/Z centre, minimum height, and low-angle quality fade.
		glm::vec4 Receiver{};
	};
	static_assert(sizeof(WeatherShadowLightingUniforms) == 112);

	struct WeatherShadowPlan {
		WeatherShadowLightingUniforms Lighting;
		core::AABB Casters;
		uint32_t Resolution = 0;
		uint32_t Cascades = 0;
		uint64_t Signature = 0;
		bool Enabled = false;
	};

	struct WeatherShadowCache {
		SDL_GPUTexture *Texture = nullptr;
		GraphHistoryGeneration Contents;
		WeatherShadowLightingUniforms Lighting;
		WeatherShadowLightingUniforms PendingLighting;

		const WeatherShadowLightingUniforms *LightingFor(const void *command) const {
			if (Contents.Pending && Contents.PendingCommand == command) return &PendingLighting;
			return Contents.Ready ? &Lighting : nullptr;
		}
		void Stage(const WeatherShadowPlan &plan, SDL_GPUTexture *texture, const void *command) {
			Texture = texture;
			PendingLighting = plan.Lighting;
			Contents.Stage(plan.Signature, command);
		}
		void Commit(const void *command) {
			if (Contents.Pending && Contents.PendingCommand == command) Lighting = PendingLighting;
			Contents.Commit(command);
		}
		void Discard(const void *command) {
			if (Contents.Pending && (!command || Contents.PendingCommand == command)) {
				// A cycled backing selected by an abandoned command cannot certify a retained map.
				Contents.Ready = false;
				Contents.Discard(command);
			}
		}
	};

	// appearanceDensity is the inherited density multiplied by clamped cosmetic cover.
	[[nodiscard]] WeatherShadowPlan BuildWeatherShadowPlan(
		const scene::WeatherCompute &controls,
		const WeatherGpuInputs &inputs,
		core::Vector3 sunDirection,
		float appearanceDensity,
		uint64_t timelineRevision,
		std::span<const WeatherResidencyView> views
	);
	// Contributes offscreen casters to the same world page union used by visible rays.
	[[nodiscard]] size_t AppendWeatherShadowDemand(
		std::span<WeatherResidencyView> views, size_t count, const WeatherShadowPlan &plan, uint64_t layerKey
	);
	[[nodiscard]] bool RecordWeatherShadows(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		SDL_GPUTexture *destination,
		const WeatherShadowPlan &plan,
		const WeatherGpuInputs &inputs,
		const scene::WeatherCompute &controls,
		float appearanceDensity
	);
}
