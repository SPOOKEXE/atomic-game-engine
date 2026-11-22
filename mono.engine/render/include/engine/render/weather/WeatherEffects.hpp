#pragma once

// arch-waiver public-header: forward presentation packet API. WorldView.hpp
// embeds WeatherEffectsFrame in camera packets consumed by client and Studio.

// Camera-local weather consumers reuse generic particles, placed fog and ribbons.
// These copied presentation values never feed the authoritative weather solver.
// @tier L12 · client

#include <engine/render/Renderer.hpp>
#include <engine/weather/Consumers.hpp>

#include <optional>
#include <span>

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	// One reusable camera packet. Cache keys describe derived presentation work.
	struct WeatherEffectsFrame {
		// Separate weather population preserves an authored generic particle field.
		std::optional<GpuParticleFieldView> Precipitation;
		// Weather response applied to a copy of authored lighting.
		scene::WorldLighting Lighting;
		// Whether a selected enabled weather provider supplied this packet.
		bool HasLighting = false;
		// Prepared camera response samples, refreshed on bounded maintenance.
		std::optional<weather::ConsumerSample> Sample;
		// Ground condensate is one separate participating medium.
		std::optional<scene::VolumeState> Fog;
		// Last rebuilt semantic packet and camera/geometry cache keys.
		//@{
		uint64_t Signature = 0;
		uint64_t World = 0;
		uint64_t Tick = 0;
		uint64_t Epoch = 0;
		uint64_t Geometry = 0;
		uint32_t Definition = 0;
		core::Vector3 CentreMetres;
		//@}
	};

	// First intersection with the published oriented opaque bounds. Mesh bounds
	// deliberately over-occlude instead of allowing precipitation through a roof.
	[[nodiscard]] std::optional<float> WeatherGeometryHit(
		std::span<const scene::DrawInstance> geometry,
		core::Vector3 originMetres,
		core::Vector3 velocityMetresPerSecond,
		float maximumSeconds
	);
	// Copies near-weather emission and terrain-relative fog; appends current
	// deterministic discharge ribbons and lights to existing authored geometry.
	void CollectWeatherEffects(
		ecs::Store &store,
		const View &view,
		WeatherEffectsFrame &frame,
		effects::RibbonBuffer &ribbons,
		std::vector<SceneLight> &lights
	);
}
