#include "InstanceResidency.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace engine::render {

	size_t InstanceKeyHash::operator()(const InstanceKey &key) const noexcept {
		size_t hash = std::hash<core::Name>{}(key.World);
		const auto mix = [&hash](uint64_t value) {
			hash ^= std::hash<uint64_t>{}(value) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
		};
		mix(key.Source);
		mix(key.Variant);
		mix(key.Fallback);
		return hash;
	}

	void InstanceResidency::BeginFrame() {
		TokenMode = false;
		Frame++;
		if (Frame == 0) {
			for (Entry &entry : Entries) {
				entry.Seen = 0;
			}
			Frame = 1;
		}
		Dirty.clear();
		Ranges.clear();
		for (Entry &entry : Entries) {
			entry.Dirty = false;
		}
	}

	void InstanceResidency::BeginFrame(uint64_t token) {
		if (TokenMode && Token == token) {
			return;
		}
		if (TokenMode && Frame > 0) {
			ReleaseUnseen();
		}
		TokenMode = true;
		Token = token;
		Frame++;
		if (Frame == 0) {
			for (Entry &entry : Entries) {
				entry.Seen = 0;
			}
			Frame = 1;
		}
	}

	uint32_t InstanceResidency::Upsert(const InstanceKey &key, const GpuInstance &row) {
		const auto found = Slots.find(key);
		if (found != Slots.end()) {
			Entry &entry = Entries[found->second];
			entry.Seen = Frame;
			if (std::memcmp(&entry.Packed, &row, sizeof(row)) != 0) {
				entry.Packed = row;
				MarkDirty(found->second);
			}
			return found->second;
		}

		uint32_t slot = std::numeric_limits<uint32_t>::max();
		while (!Free.empty()) {
			const uint32_t candidate = Free.back();
			Free.pop_back();
			if (candidate < Entries.size() && !Entries[candidate].Occupied) {
				slot = candidate;
				break;
			}
		}
		if (slot == std::numeric_limits<uint32_t>::max()) {
			slot = static_cast<uint32_t>(Entries.size());
			Entries.emplace_back();
		}

		Entry &entry = Entries[slot];
		entry.Key = key;
		entry.Packed = row;
		entry.Seen = Frame;
		entry.Occupied = true;
		Slots.emplace(key, slot);
		Live++;
		MarkDirty(slot);
		return slot;
	}

	void InstanceResidency::EndFrame() {
		if (TokenMode) {
			return;
		}
		ReleaseUnseen();
	}

	void InstanceResidency::ReleaseUnseen() {
		for (uint32_t slot = 0; slot < Entries.size(); slot++) {
			Entry &entry = Entries[slot];
			if (!entry.Occupied || entry.Seen == Frame) {
				continue;
			}
			Slots.erase(entry.Key);
			entry.Occupied = false;
			entry.Dirty = false;
			Free.push_back(slot);
			Live--;
		}

		while (!Entries.empty() && !Entries.back().Occupied) {
			Entries.pop_back();
		}
		std::erase_if(Dirty, [&](uint32_t slot) {
			return slot >= Entries.size() || !Entries[slot].Occupied;
		});
	}

	void InstanceResidency::MarkAllDirty() {
		for (uint32_t slot = 0; slot < Entries.size(); slot++) {
			if (Entries[slot].Occupied) {
				MarkDirty(slot);
			}
		}
	}

	void InstanceResidency::AcknowledgeDirty() {
		for (const uint32_t slot : Dirty) {
			if (slot < Entries.size()) {
				Entries[slot].Dirty = false;
			}
		}
		Dirty.clear();
		Ranges.clear();
	}

	std::span<const InstanceUploadRange> InstanceResidency::DirtyRanges() {
		Ranges.clear();
		if (Dirty.empty()) {
			return Ranges;
		}

		std::sort(Dirty.begin(), Dirty.end());
		uint32_t first = Dirty.front();
		uint32_t previous = first;
		for (size_t index = 1; index < Dirty.size(); index++) {
			const uint32_t slot = Dirty[index];
			if (slot == previous) {
				continue;
			}
			if (slot != previous + 1) {
				Ranges.push_back({first, previous - first + 1});
				first = slot;
			}
			previous = slot;
		}
		Ranges.push_back({first, previous - first + 1});
		return Ranges;
	}

	const GpuInstance &InstanceResidency::Row(uint32_t slot) const {
		return Entries[slot].Packed;
	}

	void InstanceResidency::MarkDirty(uint32_t slot) {
		Entry &entry = Entries[slot];
		if (entry.Dirty) {
			return;
		}
		entry.Dirty = true;
		Dirty.push_back(slot);
	}
}
