#pragma once

// Authored post-processing and distance fog beneath the Lighting service.
// Each instance contributes only while it is the first provider of its kind
// in Lighting's descendant order. The renderer receives resolved copies.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>

#include <cstdint>

namespace engine::scene {
	// Bright HDR radiance spread over nearby display pixels.
	struct BloomEffect {
		float Intensity = 0.0f;
		float Threshold = 1.0f;
		float Radius = 6.0f;
		bool Enabled = true;
		uint8_t Reserved[3] = {};
	};

	// Camera depth blur outside the sharp range.
	struct DepthOfFieldEffect {
		float Intensity = 0.0f;
		float FocusDistance = 24.0f;
		float FocusRange = 8.0f;
		float Radius = 6.0f;
		bool Enabled = true;
		uint8_t Reserved[3] = {};
	};

	// Screen-space shafts from the visible sun.
	struct GodRaysEffect {
		float Intensity = 0.0f;
		float Threshold = 1.0f;
		float Radius = 48.0f;
		bool Enabled = true;
		uint8_t Reserved[3] = {};
	};

	// Linear distance fade. A missing or disabled instance contributes no fog.
	struct FogEffect {
		core::Color3 Color{0.0f, 0.0f, 0.0f};
		float Start = 0.0f;
		float End = 100000.0f;
		bool Enabled = true;
		uint8_t Reserved[3] = {};
	};
}
