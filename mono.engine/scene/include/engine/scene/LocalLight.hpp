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
		core::Vector3 Position{};
		core::Vector3 Direction{};
		core::Color3 Colour{};
		float Range = 0.0f;
		float ConeCosine = -1.0f;
	};

	// Resolves a local light from its parent Transform or Attachment. The returned
	// value says why an authored light cannot enter the renderer's source stage.
	LocalLightRejection ResolveLocalLight(
		const ecs::Store &store, ecs::Entity entity, const Light &light, ResolvedLocalLight &resolved
	);

	const char *Describe(LocalLightRejection rejection);
}
