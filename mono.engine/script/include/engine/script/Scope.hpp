#pragma once

// Reusable cleanup scopes owned by one script runtime.
//
// The table owns lifetime and ordering only. Each VM owns the opaque resource
// references and performs their cleanup, because its callable representation
// must not cross the L9 boundary.
//
// @tier L9 · shared

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::script {

	// Resource categories released when a cleanup scope is drained.
	enum class ScopeItemKind : uint8_t {
		Callback,
		Connection,
		Entity,
		Task,
		Tween,
		Custom,
	};

	// One callback, connection, task, or custom resource registered for cleanup.
	struct ScopeItem {
		// Cleanup action the owning runtime must perform for this item.
		ScopeItemKind Kind = ScopeItemKind::Callback;
		// Runtime-owned opaque identifier passed back to that cleanup action.
		uint64_t Value = 0;
	};

	// Generation-checked reference to one live cleanup scope.
	struct ScopeHandle {
		// Slot index into ScopeTable's retained scope records.
		uint32_t Index = 0;
		// Slot generation that invalidates handles after destruction.
		uint32_t Generation = 0;
	};

	// Owns cleanup records and invalidates handles after destruction.
	class ScopeTable {
	  public:
		// Allocates an empty scope and returns its generation-checked handle.
		ScopeHandle Create();
		// Reports whether a handle still names a live scope.
		bool IsAlive(ScopeHandle handle) const;
		// Adds an item to a live cleanup scope.
		bool Add(ScopeHandle handle, ScopeItem item);
		// Removes an item from a live cleanup scope.
		bool Remove(ScopeHandle handle, ScopeItem item);
		// Counts items in a live cleanup scope.
		size_t Count(ScopeHandle handle) const;
		// Returns items while keeping the cleanup scope live.
		bool Clean(ScopeHandle handle, std::vector<ScopeItem> &items);
		// Returns items and invalidates the cleanup scope.
		bool Destroy(ScopeHandle handle, std::vector<ScopeItem> &items);

	  private:
		struct Row {
			uint32_t Generation = 1;
			bool Live = false;
			std::vector<ScopeItem> Items;
		};

		Row *Find(ScopeHandle handle);
		const Row *Find(ScopeHandle handle) const;

		std::vector<Row> Rows;
		std::vector<uint32_t> Free;
	};
}
