#pragma once

// Cosmetic fallback policy never changes canonical weather or gameplay samples.
// Its four-sample fragment approximation keeps a view drawable after compute refusal.

#include "WeatherGpu.hpp"

#include <engine/graph/RenderGraph.hpp>

namespace engine::render {
	[[nodiscard]] bool WeatherVolumeGraphAvailable(
		const graph::RenderGraph &graph, bool computeAvailable, bool programsAvailable
	);
	[[nodiscard]] bool VisibleWeatherMedium(const scene::Environment &environment);
	[[nodiscard]] bool WeatherProductsReady(const WeatherGpuGeneration *generation, const void *command);
	// Invalidates publication metadata after a partial recording; buffers stay owned for retry.
	void RefuseWeatherPublication(WeatherGpuGeneration &generation);

	struct WeatherFallbackUniforms {
		glm::vec4 Zenith, Horizon, Ground;
		glm::vec4 AirColour, AirDecay, Air;
		glm::vec4 Minimum, Maximum;
		glm::vec4 CloudColourExtinction, CloudPattern;
		glm::vec4 Sun, Direct, Ambient;
		glm::uvec4 Modes;
	};
	static_assert(sizeof(WeatherFallbackUniforms) == 224);
	[[nodiscard]] WeatherFallbackUniforms WeatherFallbackOf(
		const scene::WorldLighting &lighting,
		bool environmentTexture,
		bool drawSky,
		bool drawClouds,
		bool linearDepth,
		uint32_t faceMask
	);
}
