#pragma once

// Per-viewport LOD preview for the Components inspector.
//
// LOD choice belongs to a view, never to ECS state: the same mesh can select a
// different level in two Studio viewports. This helper keeps the inspector's
// diagnostic tied to the viewport projection it is displaying.

#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace engine::ecs {
	class Store;
}

namespace studio {
	struct PanelProjection;

	// Returns the level the focused viewport would select for a MeshPart whose
	// ladder and triangle metadata are available. A missing input has no
	// trustworthy value.
	std::optional<uint8_t> ActiveLodForViewport(
		const engine::ecs::Store &store,
		engine::ecs::Entity instance,
		const PanelProjection &panel,
		const std::array<float, 3> &distanceBands
	);
}
