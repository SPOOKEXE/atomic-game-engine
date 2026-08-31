#pragma once

// Driver-side administration of MemoryStore and DataStore values.
//
// Worlds still reach these stores only through `Postbox`. This is the copied,
// out-of-simulation view used by persistence adapters and editor tooling. It
// carries names and bytes, never references into the router's tables.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/world/Bus.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::world {

	// One copied shared-store value.
	struct SharedStoreEntry {
		BusKind Store = BusKind::MemoryStore;
		core::Name Key;
		std::vector<std::byte> Value;

		// DataStore's compare-and-set version. MemoryStore values are zero.
		uint64_t Version = 0;
	};
}
