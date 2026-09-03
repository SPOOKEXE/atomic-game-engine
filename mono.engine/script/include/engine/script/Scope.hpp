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

	enum class ScopeItemKind : uint8_t {
		Callback,
		Connection,
		Entity,
		Task,
		Tween,
		Custom,
	};

	struct ScopeItem {
		ScopeItemKind Kind = ScopeItemKind::Callback;
		uint64_t Value = 0;
	};

	struct ScopeHandle {
		uint32_t Index = 0;
		uint32_t Generation = 0;
	};

	class ScopeTable {
	  public:
		ScopeHandle Create();
		bool IsAlive(ScopeHandle handle) const;
		bool Add(ScopeHandle handle, ScopeItem item);
		bool Remove(ScopeHandle handle, ScopeItem item);
		size_t Count(ScopeHandle handle) const;
		bool Clean(ScopeHandle handle, std::vector<ScopeItem> &items);
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
