#pragma once

// Authored controls for a device-local analytical particle field.
//
// The field belongs to a scene instance and the renderer copies it into its
// presentation packet. Its positions are never a scene component: cosmetic
// state stays on the GPU and a world save contains only this small request.
//
// @tier L7 · shared

#include <cstdint>

namespace engine::scene {

	// Visible layers emitted by a GPU particle field.
	enum class GpuParticleLayer : uint8_t {
		Condensation = 1u << 0u,
		Rain = 1u << 1u,
		Debris = 1u << 2u,
	};

	// A stable bit set because this crosses saves and scripts by value.
	constexpr uint8_t GPU_PARTICLE_ALL_LAYERS = static_cast<uint8_t>(GpuParticleLayer::Condensation) |
		static_cast<uint8_t>(GpuParticleLayer::Rain) | static_cast<uint8_t>(GpuParticleLayer::Debris);

	// Requests a reusable analytical particle field for this world.
	struct GpuParticleField {
		bool Enabled = true;
		uint8_t Layers = GPU_PARTICLE_ALL_LAYERS;
		uint16_t Reserved = 0;
		// Exact supported count, normalized to the nearest safe preset by the renderer.
		uint32_t RequestedCount = 1'048'576;
		uint32_t Seed = 0xC105D00Du;
	};

	// Returns a supported count at or above the requested count, clamped to the
	// largest optional preset. Zero selects the default one-million preset.
	[[nodiscard]] uint32_t NormalizeGpuParticleCount(uint32_t requested);
	// Returns whether a layer is enabled by an authored field.
	[[nodiscard]] bool HasGpuParticleLayer(const GpuParticleField &field, GpuParticleLayer layer);
}
