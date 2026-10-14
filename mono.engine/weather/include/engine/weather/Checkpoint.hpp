#pragma once

// Bounded, versioned little-endian checkpoint codec. Decode validates before publishing any state.
// @tier L8 shared

#include <engine/weather/Simulation.hpp>

#include <cstddef>
#include <span>

namespace engine::weather {

	// Byte and cell ceilings are checked before allocation, independently of the encoded definition.
	struct CheckpointLimits {
		// Independent ceilings for encoded bytes, physical cells and forcing records.
		//@{
		uint64_t MaximumBytes = 32 * 1024 * 1024;
		uint32_t MaximumCells = 65536;
		uint32_t MaximumForcingRecords = 64;
		//@}
	};
	// Serializes all physical, topology, history and budget fields without host padding or session IDs.
	[[nodiscard]] std::optional<std::vector<std::byte>>
	EncodeCheckpoint(const SimulationState &state, CheckpointLimits limits = {});
	// Refuses incompatible, truncated, oversized or nonfinite input. The destination stays unchanged.
	[[nodiscard]] bool DecodeCheckpoint(
		std::span<const std::byte> bytes, SimulationState &destination, CheckpointLimits limits = {}
	);
}
