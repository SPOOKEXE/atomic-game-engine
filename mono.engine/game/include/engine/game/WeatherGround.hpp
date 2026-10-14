#pragma once

// Shared host adapter from completed physics queries to authoritative weather impacts.
// Function bindings stay local to their ECS store; only copied endpoints reach event codecs.
// @tier L12 · shared

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::game {
	// Installs this host's stateless downward raycast binding. Reinstall after loading world storage.
	void InstallWeatherGround(ecs::Store &store);
	// Lazily installs the binding and refreshes the physics index only when a storm impact may mature.
	// Runs after physics and before weather.runtime in the Animation phase.
	void RegisterWeatherGroundSystem(ecs::Scheduler &scheduler);
}
