#pragma once

// Demand-driven signed delivery of baked procedural regions to local worlds.
// @tier client

#include <cstddef>
#include <memory>

namespace engine::delivery {
	class AssetClient;
	class IntakeBudget;
}
namespace engine::ecs {
	class Store;
}

namespace client {
	// Cumulative installation outcomes and the number of currently active demands.
	struct ProceduralContentCounters {
		// Regions installed successfully since this service was created.
		size_t Installed = 0;
		// Regions refused because delivery or validation failed.
		size_t Refused = 0;
		// Regions with outstanding description or mesh delivery work.
		size_t Pending = 0;
	};
	// Tracks and installs signed baked procedural content for local worlds.
	class ProceduralContent {
	  public:
		ProceduralContent();
		~ProceduralContent();
		ProceduralContent(const ProceduralContent &) = delete;
		ProceduralContent &operator=(const ProceduralContent &) = delete;
		// Starts frame tracking so unseen region demands can be removed at frame end.
		void BeginFrame();
		// Advances at most the current world demand using the shared intake budget.
		void
		PumpWorld(engine::ecs::Store &, engine::delivery::AssetClient &, engine::delivery::IntakeBudget &);
		// Drops demands not encountered during this frame and cancels their requests.
		void EndFrame(engine::delivery::AssetClient &);
		// Returns cumulative installed/refused counts and the current pending count.
		ProceduralContentCounters Counters() const;

	  private:
		struct State;
		std::unique_ptr<State> Held;
	};
}
