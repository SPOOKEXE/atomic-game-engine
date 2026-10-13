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
		// Bloom contribution strength; zero disables the pass.
		float Intensity = 0.0f;

		// HDR brightness above which pixels contribute to bloom.
		float Threshold = 1.0f;

		// Sampling offset for the spread, in display pixels.
		float Radius = 6.0f;

		// Apply this row; first provider wins even when disabled.
		bool Enabled = true;

		// Reserved padding; keep these bytes zero.
		uint8_t Reserved[3] = {};
	};

	// Camera depth blur outside the sharp range.
	struct DepthOfFieldEffect {
		// Blur strength from zero to one; zero disables the pass.
		float Intensity = 0.0f;

		// Camera distance to the center of the sharp range, in world units.
		float FocusDistance = 24.0f;

		// Sharp distance on either side of FocusDistance, in world units.
		float FocusRange = 8.0f;

		// Blur sampling radius, in display pixels.
		float Radius = 6.0f;

		// Apply this row; first provider wins even when disabled.
		bool Enabled = true;

		// Reserved padding; keep these bytes zero.
		uint8_t Reserved[3] = {};
	};

	// Screen-space shafts from the visible sun.
	struct GodRaysEffect {
		// Shaft contribution strength; zero disables the pass.
		float Intensity = 0.0f;

		// HDR brightness above which sampled pixels contribute to shafts.
		float Threshold = 1.0f;

		// Maximum shaft sampling distance, in display pixels.
		float Radius = 48.0f;

		// Apply this row; first provider wins even when disabled.
		bool Enabled = true;

		// Reserved padding; keep these bytes zero.
		uint8_t Reserved[3] = {};
	};

	// Linear distance fade. A missing or disabled instance uses the far no-fog range.
	struct FogEffect {
		// Fog tint applied at the far end of the fade.
		core::Color3 Color{0.0f, 0.0f, 0.0f};

		// Camera distance where fog begins, in world units.
		float Start = 0.0f;

		// Camera distance where fog reaches full strength, in world units.
		float End = 100000.0f;

		// Apply this row; first provider wins even when disabled.
		bool Enabled = true;

		// Reserved padding; keep these bytes zero.
		uint8_t Reserved[3] = {};
	};
}
