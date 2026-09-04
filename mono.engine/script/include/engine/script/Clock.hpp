#pragma once

// The independent cadence for script barriers within one world.

#include <cstdint>
#include <limits>
#include <optional>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// The state that turns fixed world ticks into script updates.
	struct ScriptClock {
		double Rate = 0.0;
		double Accumulator = 0.0;
		uint64_t ObservedTick = std::numeric_limits<uint64_t>::max();
	};

	// Sets the script update rate. Zero follows every world tick.
	void SetScriptTickRate(ecs::Store &store, double updatesPerSecond);

	// Returns this barrier's script delta, or nothing when it is not due.
	std::optional<float> TakeScriptUpdate(ecs::Store &store);
}
