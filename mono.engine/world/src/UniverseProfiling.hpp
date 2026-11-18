#pragma once

#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/world/World.hpp>

#include <span>

namespace engine::world {
	struct WorldProfiling {
		enum Phase : size_t { Setup, Prepare, Commit };

		static const std::array<float, 3> &TickPhases(const World &world) {
			return world.TickPhaseMilliseconds;
		}
	};

	// Keeps the existing profiling zone's producer duration when its live
	// FrameGraph span cannot be recorded on the frame-owning thread.
	class TickPhaseTimer {
	  public:
		explicit TickPhaseTimer(float &milliseconds)
			: Milliseconds(milliseconds),
			  Started(core::FrameGraph::IsEnabled() ? core::Clock::Nanoseconds() : 0) {}
		~TickPhaseTimer() {
			if (Started != 0) {
				Milliseconds += static_cast<float>(core::Clock::Nanoseconds() - Started) / 1'000'000.0f;
			}
		}
		TickPhaseTimer(const TickPhaseTimer &) = delete;
		TickPhaseTimer &operator=(const TickPhaseTimer &) = delete;

	  private:
		float &Milliseconds;
		uint64_t Started;
	};

	// Rebuilds a worker-owned scheduler subtree after that worker has joined the
	// frame owner. The timing rows are already restricted to the work being
	// reported, so catch-up rounds cannot repeat a prior round.
	void ReportWorkerSchedulerTimings(
		World &world,
		std::span<const ecs::Scheduler::Timing> timings,
		float worldMilliseconds,
		const std::array<float, 3> &tickPhaseMilliseconds
	);
}
