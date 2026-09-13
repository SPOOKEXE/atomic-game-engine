#pragma once

// Client-owned installation of systems every local world needs after load.
// @tier client

#include <optional>

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace client {
	// Prepares missing physics resources at startup and installs local simulation systems.
	// An existing clock is serialized world state and is never reset.
	void InstallClientWorldSystems(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &systems,
		std::optional<double> initialPhysicsTickRate = std::nullopt
	);
}
