#pragma once

#include <engine/ecs/Entity.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	// Clones StarterGui into PlayerGui and gives cloned live graphs unique keys.
	// Rewrites named image references inside the cloned subtree to those keys.
	// Hosts use this entry point so graph instances in different players do not
	// compete for the same world-local content name.
	//
	// @param store The world containing StarterGui and the player.
	// @param player The player whose PlayerGui is rebuilt.
	// @return The number of top-level StarterGui children successfully cloned.
	// @tier L9 · shared
	size_t ResetPlayerGui(ecs::Store &store, ecs::Entity player);
}
