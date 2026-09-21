#pragma once

// Per-viewport LOD preview for the Components inspector.
//
// LOD choice belongs to a view, never to ECS state: the same mesh can select a
// different level in two Studio viewports. This helper keeps the inspector's
// diagnostic tied to the viewport projection it is displaying.

#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct LODSettings;
}

namespace studio {
	struct PanelProjection;

	// Places a label so its horizontal midpoint matches the projected object's.
	float CenteredLodLabelX(float objectMinimumX, float objectMaximumX, float labelWidth);

	// Whether the viewport overlay should identify this visible MeshPart. LOD 0
	// is still useful before a policy exists or while its artifacts are building.
	bool ShouldDrawActiveLodLabel(const engine::ecs::Store &store, engine::ecs::Entity instance);

	// Resolves an optional per-item distance override against Studio's view defaults.
	// An incomplete or unordered override cannot change a subset of the selector,
	// so it inherits all three defaults.
	std::array<float, 3> EffectiveLodDistanceBands(
		const engine::scene::LODSettings *settings, const std::array<float, 3> &defaultBands
	);

	// Produces the complete triplet a per-item distance edit must commit. An
	// inherited or malformed row is materialized from the current preferences
	// before one field changes, so the renderer never rejects a partial edit.
	std::array<float, 3> EditedLodDistanceBands(
		const engine::scene::LODSettings *settings,
		const std::array<float, 3> &defaultBands,
		size_t level,
		float distance
	);

	// Returns the level the focused viewport would select for a MeshPart. A
	// partially resident ladder is limited to its available prefix; a missing
	// base mesh or view input has no trustworthy value.
	std::optional<uint8_t> ActiveLodForViewport(
		const engine::ecs::Store &store,
		engine::ecs::Entity instance,
		const PanelProjection &panel,
		const std::array<float, 3> &distanceBands
	);
}
