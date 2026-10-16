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
	struct ProceduralContentCounters {
		size_t Installed = 0;
		size_t Refused = 0;
		size_t Pending = 0;
	};
	class ProceduralContent {
	  public:
		ProceduralContent();
		~ProceduralContent();
		ProceduralContent(const ProceduralContent &) = delete;
		ProceduralContent &operator=(const ProceduralContent &) = delete;
		void BeginFrame();
		void
		PumpWorld(engine::ecs::Store &, engine::delivery::AssetClient &, engine::delivery::IntakeBudget &);
		void EndFrame(engine::delivery::AssetClient &);
		ProceduralContentCounters Counters() const;

	  private:
		struct State;
		std::unique_ptr<State> Held;
	};
}
