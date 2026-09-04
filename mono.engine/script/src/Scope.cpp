#include <engine/script/Scope.hpp>

#include <algorithm>

namespace engine::script {

	ScopeHandle ScopeTable::Create() {
		if (!Free.empty()) {
			const uint32_t index = Free.back();
			Free.pop_back();
			Row &row = Rows[index];
			row.Live = true;
			return {index, row.Generation};
		}

		Rows.emplace_back();
		Row &row = Rows.back();
		row.Live = true;
		return {static_cast<uint32_t>(Rows.size() - 1), row.Generation};
	}

	bool ScopeTable::IsAlive(ScopeHandle handle) const {
		return Find(handle) != nullptr;
	}

	bool ScopeTable::Add(ScopeHandle handle, ScopeItem item) {
		Row *row = Find(handle);
		if (row == nullptr) {
			return false;
		}
		row->Items.push_back(item);
		return true;
	}

	bool ScopeTable::Remove(ScopeHandle handle, ScopeItem item) {
		Row *row = Find(handle);
		if (row == nullptr) {
			return false;
		}
		const auto found = std::find_if(row->Items.begin(), row->Items.end(), [&](const ScopeItem &held) {
			return held.Kind == item.Kind && held.Value == item.Value;
		});
		if (found == row->Items.end()) {
			return false;
		}
		// Keep insertion order intact. Cleaners run newest first, and swapping
		// the final item into this slot would make that observable after Remove.
		row->Items.erase(found);
		return true;
	}

	size_t ScopeTable::Count(ScopeHandle handle) const {
		const Row *row = Find(handle);
		return row == nullptr ? 0 : row->Items.size();
	}

	bool ScopeTable::Clean(ScopeHandle handle, std::vector<ScopeItem> &items) {
		Row *row = Find(handle);
		if (row == nullptr) {
			return false;
		}
		items.swap(row->Items);
		return true;
	}

	bool ScopeTable::Destroy(ScopeHandle handle, std::vector<ScopeItem> &items) {
		Row *row = Find(handle);
		if (row == nullptr) {
			return false;
		}
		items.swap(row->Items);
		row->Live = false;
		row->Generation++;
		Free.push_back(handle.Index);
		return true;
	}

	ScopeTable::Row *ScopeTable::Find(ScopeHandle handle) {
		if (handle.Index >= Rows.size()) {
			return nullptr;
		}
		Row &row = Rows[handle.Index];
		return row.Live && row.Generation == handle.Generation ? &row : nullptr;
	}

	const ScopeTable::Row *ScopeTable::Find(ScopeHandle handle) const {
		return const_cast<ScopeTable *>(this)->Find(handle);
	}
}
