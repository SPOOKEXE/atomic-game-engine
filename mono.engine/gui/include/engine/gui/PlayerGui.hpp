#pragma once

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Stable template identity lets a retained local collector adopt a fresh server copy.
	struct PlayerGuiTemplateOrigin {
		// Authored instance path used to match the collector across copies.
		core::Name Path;
	};

	// Canonical presentation sources stay available to server scripts in shared hosts.
	// These viewer-local facets are cleared by serialization.
	struct PlayerGuiSource {
		// Whether this collector is an active canonical source for projection.
		bool Active = false;
		// Viewer-local copy, or null until projection creates one.
		ecs::Entity Copy;
	};

	// Projection metadata that maps a viewer-local copy to its authored source.
	using PlayerGuiCopy = ecs::InstanceProjection;

	// Updates owned local GUI copies after authoritative rows are adopted. A new
	// non-null character replaces only collectors whose local ResetOnSpawn is true.
	size_t RefreshPlayerGuiProjection(ecs::Store &store, ecs::Entity player, ecs::Entity character);

	// Includes descendants of a tagged source or local copy.
	bool IsPlayerGuiSource(const ecs::Store &store, ecs::Entity instance);
	// Reports whether an instance belongs to a projected viewer-local copy.
	bool IsPlayerGuiCopy(const ecs::Store &store, ecs::Entity instance);

	// Exact node mapping. A locally destroyed copy has no replacement until respawn.
	ecs::Entity FindPlayerGuiCopy(const ecs::Store &store, ecs::Entity source);
	// Returns the authored source paired with this viewer-local copy, or null.
	ecs::Entity PlayerGuiSourceOf(const ecs::Store &store, ecs::Entity copy);
}
