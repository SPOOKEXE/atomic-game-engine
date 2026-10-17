#pragma once

namespace engine::ecs {
	class Store;
	class Scheduler;
}

namespace client {
	// Device particle requests and local trail/ribbon presentation, without physics.
	void InstallEffectsPresentation(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);
}
