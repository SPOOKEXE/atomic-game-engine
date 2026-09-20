#pragma once

// A placed participating medium for the renderer.
//
// A Volume is deliberately just authored density data and a transform. The
// renderer resolves it into a value payload before recording, so physics and
// gameplay never gain a rendering dependency and no world pointer crosses the
// presentation boundary.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The density boundary of a placed medium. The default box preserves the
	// usual room fog use case, while an ellipsoid gives authors a smooth bounded
	// cloud without a mesh or a special renderer.
	enum class VolumeShape : uint8_t {
		Box = 0,
		Ellipsoid = 1,
	};

	// One authored local volume. Density is procedurally varied by the renderer
	// today, but the bounds, extinction and scattering terms are resource data
	// rather than renderer-specific cloud controls.
	struct Volume {
		// Scattered light colour before density and extinction are applied.
		core::Color3 Colour{0.82f, 0.86f, 0.92f};
		// Local half-size in metres for the box or ellipsoid boundary.
		core::Vector3 HalfExtent{12.0f, 8.0f, 12.0f};
		// Base participating-medium density in local volume space.
		float Density = 0.18f;
		// Per-distance light loss through the medium.
		float Extinction = 0.45f;
		// Edge density fade, where zero keeps a hard authored boundary.
		float Falloff = 0.0f;
		// Local-space frequency of procedural density variation.
		float NoiseScale = 0.18f;
		// Amplitude of procedural density variation.
		float NoiseStrength = 0.7f;
		// Maximum view-ray integration steps through this volume.
		uint32_t Steps = 24;
		// Maximum shadow-ray integration steps through this volume.
		uint32_t ShadowSteps = 8;
		// Stable procedural-noise seed.
		uint32_t Seed = 1;
		// Boundary geometry used to test local volume membership.
		VolumeShape Shape = VolumeShape::Box;
		// Whether the volume contributes to resolved presentation state.
		bool Enabled = true;
		// Explicit padding retained for the fixed presentation record layout.
		uint8_t Reserved[2] = {};
	};

	// The fixed-size snapshot a renderer receives for one authored volume.
	// `Frame` remains an engine value, not a pointer into the world.
	struct VolumeState {
		// World transform of the authored local boundary.
		core::CFrame Frame;
		// Resolved scattered-light colour.
		core::Color3 Colour{0.82f, 0.86f, 0.92f};
		// Resolved local half-size in metres.
		core::Vector3 HalfExtent{12.0f, 8.0f, 12.0f};
		// Resolved base medium density.
		float Density = 0.18f;
		// Resolved per-distance light loss.
		float Extinction = 0.45f;
		// Resolved edge density fade.
		float Falloff = 0.0f;
		// Resolved procedural density frequency.
		float NoiseScale = 0.18f;
		// Resolved procedural density amplitude.
		float NoiseStrength = 0.7f;
		// Resolved view-ray integration limit.
		uint32_t Steps = 24;
		// Resolved shadow-ray integration limit.
		uint32_t ShadowSteps = 8;
		// Resolved stable procedural-noise seed.
		uint32_t Seed = 1;
		// Resolved boundary geometry.
		VolumeShape Shape = VolumeShape::Box;
		// Whether this snapshot slot contains an enabled volume.
		bool Enabled = false;
	};

	// Copies enabled placed volumes into `out` in stable entity order. The fixed
	// cap makes the screen-space cost explicit and keeps the presentation copy
	// bounded regardless of authored hierarchy size.
	size_t ResolveVolumes(const ecs::Store &store, std::span<VolumeState> out);
}
