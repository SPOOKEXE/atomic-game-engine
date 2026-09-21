#pragma once

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Pipeline-only writers for the read-only observation resource.
	void RegisterPhysicsObservations();
	void PreparePhysicsObservations(ecs::Store &store);
	void RecordPostIntegration(ecs::Store &store);
	void RecordPreSolve(ecs::Store &store);
	void RecordCompletedSolver(ecs::Store &store);
}
