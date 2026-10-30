#pragma once

// Factory types for host-bound script compute endpoints.
// @tier L9 · shared

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine::script {
	class ComputeBridge;
	// Runtime calls this once with its actual VM identity and ECS storage incarnation.
	using ComputeBridgeFactory = std::function<std::shared_ptr<ComputeBridge>(uint64_t, uint64_t)>;
	// Host creates a fixed world/view factory before entering its script world.
	using ComputeWorldBridgeFactory = std::function<ComputeBridgeFactory(std::string)>;
}
