#pragma once

// Owner-thread placement sweep used by the scheduled copied-contact solver.
// Public placement queries remain const, unlimited and safe for concurrent reads.
#include <engine/physics/Query.hpp>

namespace engine::physics {
	PlacementSweep SweepPlacementBudgeted(
		ecs::Store &store,
		const scene::Collider &collider,
		const core::CFrame &from,
		const core::Vector3 &displacement,
		const core::Vector3 &angularDisplacement,
		ecs::Entity ignore,
		bool blockingOnly
	);
}
