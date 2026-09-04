#pragma once

// Script-facing structural destruction groups.
//
// A group names the authored collection that may be released together. It does
// not decide why a piece broke or calculate damage. Those policies sit above
// scene and can call this primitive when they have made that decision.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Releases every anchored BasePart below a BreakGroup into simulation.
	// Existing dynamic pieces are retained untouched, so repeated calls are
	// idempotent and a larger group can safely contain a smaller one.
	//
	// @return The number of parts released, or zero when `group` is not a
	//         BreakGroup or has no anchored parts.
	size_t ReleaseBreakGroup(ecs::Store &store, ecs::Entity group);
}
