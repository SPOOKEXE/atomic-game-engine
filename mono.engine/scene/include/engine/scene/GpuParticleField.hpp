#pragma once

// Bounded authored samples for device-local cosmetic particles. ECS owns the
// samples and styles; live positions remain on the GPU and never enter a save.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Independently selectable generic visual groups.
	enum class GpuParticleLayer : uint8_t {
		// First generic visual group.
		First = 1u,
		// Second generic visual group.
		Second = 2u,
		// Third generic visual group.
		Third = 4u
	};
	// All supported visual groups.
	inline constexpr uint8_t GPU_PARTICLE_ALL_LAYERS = 7;
	// Maximum authored samples retained by one field.
	inline constexpr size_t MAX_GPU_PARTICLE_SPAWN_SAMPLES = 8192;
	// Packed sample size: position, lifetime, velocity and group index.
	inline constexpr size_t GPU_PARTICLE_SPAWN_SAMPLE_BYTES = 32;

	// One reusable local-space initial condition, not a live particle row.
	struct GpuParticleSpawnSample {
		// Position relative to the field's transform.
		core::Vector3 Position;
		// Positive lifetime in seconds, at most one hour.
		float Lifetime = 15.0f;
		// Initial local-space velocity in world units per second.
		core::Vector3 Velocity;
		// Visual group index in 0..2.
		uint32_t Layer = 0;
		// Compares complete authored initial conditions.
		bool operator==(const GpuParticleSpawnSample &) const = default;
	};

	// Generic appearance and constant load for one visual group.
	struct GpuParticleStyle {
		// Linear RGB tint.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};
		// Straight-alpha opacity in 0..1.
		float Alpha = 0.1f;
		// Billboard width and height in world units, at most 64.
		float Size = 1.0f;
		// Constant world-space acceleration, independent of the selected vector field.
		core::Vector3 Acceleration;
		// Compares complete visual and acceleration settings.
		bool operator==(const GpuParticleStyle &) const = default;
	};

	// Requests a device-local population driven by the nearest vector-field ancestor.
	struct GpuParticleField {
		// Whether the field is simulated and drawn.
		bool Enabled = true;
		// Enabled visual groups as a three-bit mask.
		uint8_t Layers = GPU_PARTICLE_ALL_LAYERS;
		// Desired population, normalized to supported presets through fifty million.
		uint32_t RequestedCount = 1'048'576;
		// Deterministic sample-selection seed.
		uint32_t Seed = 0xC105D00Du;
		// Local-space recycling bounds around the field transform.
		core::Vector3 HalfExtent{512.0f, 512.0f, 512.0f};
		// Rate of following the sampled vector's target velocity; zero adds no following.
		float VelocityResponse = 2.5f;
		// Appearance and constant acceleration of the three generic groups.
		std::array<GpuParticleStyle, 3> Styles{};
		// Bounded owned initial conditions shared by the whole device population.
		std::vector<GpuParticleSpawnSample> SpawnSamples;
	};

	// Returns the next supported population, clamped to fifty million; zero selects one million.
	[[nodiscard]] uint32_t NormalizeGpuParticleCount(uint32_t requested);
	// Reports whether the complete field and its bounded initial conditions are valid.
	[[nodiscard]] bool ValidGpuParticleField(const GpuParticleField &field);
	// Reports whether a generic visual group is enabled.
	[[nodiscard]] bool HasGpuParticleLayer(const GpuParticleField &field, GpuParticleLayer layer);
	// Replaces packed little-endian samples transactionally; empty bytes clear the population request.
	bool SetGpuParticleSpawnSamples(ecs::Store &, ecs::Entity, std::span<const std::byte> bytes);
	// Replaces one generic style transactionally, refusing invalid values and authority-owned replicas.
	bool SetGpuParticleStyle(ecs::Store &, ecs::Entity, uint32_t index, const GpuParticleStyle &);
	// Encodes styles and samples for authored saves without including device-local live state.
	std::string GpuParticleDefinition(const GpuParticleField &);
	// Decodes a bounded authored definition into a candidate, preserving the caller's value on refusal.
	bool ReadGpuParticleDefinition(std::string_view, GpuParticleField &);
}
