#pragma once

// arch-waiver public-header: forward world API. Multi-world hosts use this
// complete shared-store contract rather than copying world ownership state.

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
		// Which shared store owns the row.
		BusKind Store = BusKind::MemoryStore;

		// Stable string-backed key.
		core::Name Key;

		// Opaque encoded value bytes.
		std::vector<std::byte> Value;

		// DataStore's compare-and-set version. MemoryStore values are zero.
		uint64_t Version = 0;

		// Compares every persisted field.
		bool operator==(const SharedStoreEntry &) const = default;
	};
}
