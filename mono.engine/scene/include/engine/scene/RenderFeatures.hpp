#pragma once

// Authored rendering policy shared by worlds, cameras and visual instances.
//
// Device support is deliberately absent. A scene says what it wants here;
// the renderer intersects that request with the selected pipeline and the
// probed device before a shader sees it. Keeping the two separate prevents a
// saved world from claiming that the machine loading it has a feature.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::scene {

	// Stable feature bits stored in snapshots and GPU rows. Values are append-only.
	//
	// @since v0.24
	enum class RenderFeature : uint32_t {
		Shadows = 1u << 0u,
		AmbientOcclusion = 1u << 1u,
		Emission = 1u << 2u,
		Reflections = 1u << 3u,
		Refraction = 1u << 4u,
		MotionVectors = 1u << 5u,
		RayTracing = 1u << 6u,
		OcclusionCulling = 1u << 7u,
		TwoSided = 1u << 8u,
		Displacement = 1u << 9u,
		ComputeEffects = 1u << 10u,
		PostProcessing = 1u << 11u,
	};

	inline constexpr uint32_t ALL_RENDER_FEATURES = (1u << 12u) - 1u;

	constexpr uint32_t FeatureBit(RenderFeature feature) {
		return static_cast<uint32_t>(feature);
	}

	// One tri-state policy. A bit in neither mask inherits, Enable requests it,
	// and Disable refuses it. Disable wins when malformed input sets both.
	//
	// @since v0.24
	struct RenderFeaturePolicy {
		uint32_t Enable = 0;
		uint32_t Disable = 0;
	};

	// Applies one authored layer to a resolved feature set.
	constexpr uint32_t ApplyRenderFeaturePolicy(uint32_t inherited, RenderFeaturePolicy policy) {
		const uint32_t disabled = policy.Disable & ALL_RENDER_FEATURES;
		return ((inherited | (policy.Enable & ALL_RENDER_FEATURES)) & ~disabled) & ALL_RENDER_FEATURES;
	}

	// World, camera and instance policy in precedence order, constrained by what
	// the selected renderer path supports. Refused contains authored requests
	// removed by support and is suitable for diagnostics without a GPU readback.
	struct ResolvedRenderFeatures {
		uint32_t Enabled = 0;
		uint32_t Refused = 0;
	};

	constexpr ResolvedRenderFeatures ResolveRenderFeatures(
		uint32_t defaults,
		RenderFeaturePolicy world,
		RenderFeaturePolicy camera,
		RenderFeaturePolicy instance,
		uint32_t supported
	) {
		const uint32_t requested = ApplyRenderFeaturePolicy(
			ApplyRenderFeaturePolicy(ApplyRenderFeaturePolicy(defaults, world), camera), instance
		);
		const uint32_t available = supported & ALL_RENDER_FEATURES;
		return {requested & available, requested & ~available & ALL_RENDER_FEATURES};
	}

	// An authored graph node attached to one visual item. Shader source and typed
	// parameters remain on the pipeline node; this record selects that stable
	// node and supplies per-item ordering and selection data.
	//
	// @since v0.24
	enum class RenderEffectStage : uint8_t {
		Compute = 0,
		PostProcess = 1,
	};

	inline constexpr size_t MAX_RENDER_EFFECT_ATTACHMENTS = 4;

	struct RenderEffectAttachment {
		core::Name Node;
		uint32_t SelectionMask = UINT32_MAX;
		uint32_t Order = 0;
		uint32_t Revision = 0;
		RenderEffectStage Stage = RenderEffectStage::PostProcess;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	// Optional ECS column placed only on visuals that attach graph work.
	struct RenderEffects {
		std::array<RenderEffectAttachment, MAX_RENDER_EFFECT_ATTACHMENTS> Attachments{};
		uint8_t Count = 0;
		uint8_t Reserved[7] = {};
	};
}
