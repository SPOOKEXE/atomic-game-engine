#pragma once

// The source-stage local-light answer shared by scene observers and presentation.
//
// Portal transport and camera budgeting are view work. This resolver stops before
// either so every consumer agrees on the authored source light.

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct Light;

	// Reason an authored light cannot produce a renderer source row.
	enum class LocalLightRejection : uint8_t {
		None,
		Disabled,
		NonPositiveBrightness,
		NonPositiveRange,
		MissingParent,
		ParentHasNoPlacement,
	};

	// The renderer's source row before portal duplication and the camera light cap.
	// Colour has brightness folded in because that is the RGB the renderer consumes.
	struct ResolvedLocalLight {
		// World-space emission origin.
		core::Vector3 Position{};
		// World-space direction for a spot or surface light.
		core::Vector3 Direction{};
		// Linear RGB emission with brightness already applied.
		core::Color3 Colour{};
		// Hard illumination cutoff in metres.
		float Range = 0.0f;
		// Cosine of the spot or surface cone half-angle.
		float ConeCosine = -1.0f;
	};

	// Resolves a local light from its parent Transform or Attachment. The returned
	// value says why an authored light cannot enter the renderer's source stage.
	LocalLightRejection ResolveLocalLight(
		const ecs::Store &store, ecs::Entity entity, const Light &light, ResolvedLocalLight &resolved
	);

	// Returns a stable diagnostic name for a rejection reason.
	const char *Describe(LocalLightRejection rejection);
}
