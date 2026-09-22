#pragma once

#include <engine/world/TickExchange.hpp>

#include <functional>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}
namespace engine::world {
	class Universe;
}

namespace engine::game {
	// Runs the fixed-step seam barrier. All cross-world facts are opaque copied
	// records; only the coordinator's resolver knows their physics meaning.
	class PortalSeamCoordinator {
	  public:
		// Collects one world's opaque seam facts.
		using Collect = std::function<void(ecs::Store &, std::vector<std::byte> &)>;
		// Resolves copied facts into records for their destination worlds.
		using Resolve = std::function<bool(
			std::span<const world::FixedStepBarrierRecord>, std::vector<world::FixedStepBarrierRecord> &
		)>;
		// Applies resolved opaque facts to one world.
		using Apply = std::function<bool(ecs::Store &, std::span<const std::byte>)>;

		// Creates a coordinator from the host's seam callbacks.
		PortalSeamCoordinator(Collect collect, Resolve resolve, Apply apply);

		// Advances a locally hosted frame through the ordered seam barrier. A
		// process host may drive the public Universe stages separately and invoke
		// Resolve on its copied records over its existing host link.
		bool Tick(world::Universe &worlds, float frameSeconds);
		// Supplies the generic world driver with the local collect/apply hooks and
		// the driver-owned global resolver.
		const world::FixedStepBarrierCallbacks &Callbacks() const {
			return Barrier;
		}

	  private:
		world::FixedStepBarrierCallbacks Barrier;
	};

	// The default local-host seam driver. Its packets are portable, so a process
	// host can use the same collect, resolve, and apply functions at its own
	// fixed-step barrier.
	PortalSeamCoordinator MakePortalIslandCoordinator();
}
