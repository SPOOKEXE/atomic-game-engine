#pragma once

// World-space image lenses resolved before rendering.
//
// A ShaderLens is authored spatial data, not a render pass. It names a lens
// shader and describes the spherical region that shader may affect; the
// renderer receives a bounded value snapshot and owns all device resources.
// This keeps a headless world meaningful and keeps no world pointer alive in a
// presentation host.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The first lens boundary. More shapes need a renderer contract and a
	// visible use case before they become authored vocabulary.
	enum class LensShape : uint8_t {
		Sphere = 0,
	};

	// One placed region that warps the completed HDR scene behind it.
	struct ShaderLens {
		// The lens-program name. It is written as text, never as Name::Id().
		core::Name Shader;

		// Outer world-space radius of the affected sphere.
		float Radius = 16.0f;

		// The opaque core radius. The resolver keeps it within Radius.
		float InnerRadius = 4.0f;

		// Edge transition from the outer radius inward, from zero to one.
		float Falloff = 0.75f;

		// Shader-independent intensity supplied to every lens program.
		float Strength = 1.0f;

		// Rotation around the view ray in radians per second.
		float Spin = 0.0f;

		// Later values compose over earlier ones when lenses overlap.
		int32_t Priority = 0;

		LensShape Shape = LensShape::Sphere;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	// A value-only lens record handed across the world-presentation boundary.
	struct ShaderLensState {
		core::CFrame Frame;
		core::Name Shader;
		// A stable local tie-breaker for equal priority and shader names. It never
		// leaves the world; the renderer only uses it to make bounded selection
		// repeatable while an author has overlapping lenses.
		uint64_t EntityId = 0;
		float Radius = 0.0f;
		float InnerRadius = 0.0f;
		float Falloff = 0.0f;
		float Strength = 0.0f;
		float Spin = 0.0f;
		int32_t Priority = 0;
		LensShape Shape = LensShape::Sphere;
	};

	// A screen-space lens costs work per pixel, so the presentation snapshot is
	// deliberately bounded even when an author has placed more instances.
	inline constexpr size_t MAX_SCENE_SHADER_LENSES = 16;

	// Resolves enabled, valid lenses in deterministic priority order. When the
	// authored set exceeds `out`, the highest-priority entries win.
	size_t ResolveShaderLenses(const ecs::Store &store, std::span<ShaderLensState> out);

	// Collects distinct lens shader names in stable Name-id order.
	size_t DemandedLensShaders(const ecs::Store &store, std::vector<core::Name> &out);
}
