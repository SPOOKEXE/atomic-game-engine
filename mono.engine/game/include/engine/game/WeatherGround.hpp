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
	// Prepares accepted terrain snapshots after weather.clock and before weather.stateful, and refreshes
	// impact queries after physics before weather.runtime. Steady terrain does not rebuild its index.
	void RegisterWeatherGroundSystem(ecs::Scheduler &scheduler);
}
