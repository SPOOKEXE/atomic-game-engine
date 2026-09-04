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
		core::Color3 Colour{0.82f, 0.86f, 0.92f};
		core::Vector3 HalfExtent{12.0f, 8.0f, 12.0f};
		float Density = 0.18f;
		float Extinction = 0.45f;
		float Falloff = 0.0f;
		float NoiseScale = 0.18f;
		float NoiseStrength = 0.7f;
		uint32_t Steps = 24;
		uint32_t ShadowSteps = 8;
		uint32_t Seed = 1;
		VolumeShape Shape = VolumeShape::Box;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	// The fixed-size snapshot a renderer receives for one authored volume.
	// `Frame` remains an engine value, not a pointer into the world.
	struct VolumeState {
		core::CFrame Frame;
		core::Color3 Colour{0.82f, 0.86f, 0.92f};
		core::Vector3 HalfExtent{12.0f, 8.0f, 12.0f};
		float Density = 0.18f;
		float Extinction = 0.45f;
		float Falloff = 0.0f;
		float NoiseScale = 0.18f;
		float NoiseStrength = 0.7f;
		uint32_t Steps = 24;
		uint32_t ShadowSteps = 8;
		uint32_t Seed = 1;
		VolumeShape Shape = VolumeShape::Box;
		bool Enabled = false;
	};

	// Copies enabled placed volumes into `out` in stable entity order. The fixed
	// cap makes the screen-space cost explicit and keeps the presentation copy
	// bounded regardless of authored hierarchy size.
	size_t ResolveVolumes(const ecs::Store &store, std::span<VolumeState> out);
}
