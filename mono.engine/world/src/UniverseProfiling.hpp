#pragma once

#include <engine/ecs/Scheduler.hpp>

#include <span>

namespace engine::world {
	class World;

	// Rebuilds a worker-owned scheduler subtree after that worker has joined the
	// frame owner. The timing rows are already restricted to the work being
	// reported, so catch-up rounds cannot repeat a prior round.
	void ReportWorkerSchedulerTimings(
		World &world, std::span<const ecs::Scheduler::Timing> timings, float worldMilliseconds
	);
}
