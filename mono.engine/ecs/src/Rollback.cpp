#include <engine/ecs/Rollback.hpp>

#include <algorithm>

namespace engine::ecs {
	RollbackBuffer::RollbackBuffer(size_t capacity) : EntryCapacity(capacity) {
		Entries.reserve(capacity);
	}

	bool RollbackBuffer::Capture(const Store &store, uint64_t tick) {
		if (EntryCapacity == 0) {
			return false;
		}

		core::ByteWriter writer;
		if (!store.Save(writer)) {
			return false;
		}

		const auto firstReplaced =
			std::lower_bound(Entries.begin(), Entries.end(), tick, [](const Entry &entry, uint64_t value) {
				return entry.Tick < value;
			});
		Entries.erase(firstReplaced, Entries.end());

		const std::span<const std::byte> bytes = writer.Bytes();
		Entries.push_back(Entry{tick, {bytes.begin(), bytes.end()}});
		if (Entries.size() > EntryCapacity) {
			Entries.erase(Entries.begin());
		}
		return true;
	}

	bool RollbackBuffer::Restore(Store &store, uint64_t tick) const {
		const auto found =
			std::lower_bound(Entries.begin(), Entries.end(), tick, [](const Entry &entry, uint64_t value) {
				return entry.Tick < value;
			});
		if (found == Entries.end() || found->Tick != tick) {
			return false;
		}

		core::ByteReader reader(found->Snapshot);
		return store.Load(reader);
	}

	void RollbackBuffer::DiscardAfter(uint64_t tick) {
		const auto firstDiscarded =
			std::upper_bound(Entries.begin(), Entries.end(), tick, [](uint64_t value, const Entry &entry) {
				return value < entry.Tick;
			});
		Entries.erase(firstDiscarded, Entries.end());
	}

	size_t RollbackBuffer::Size() const {
		return Entries.size();
	}

	size_t RollbackBuffer::Capacity() const {
		return EntryCapacity;
	}

	std::optional<uint64_t> RollbackBuffer::OldestTick() const {
		if (Entries.empty()) {
			return std::nullopt;
		}
		return Entries.front().Tick;
	}

	std::optional<uint64_t> RollbackBuffer::LatestTick() const {
		if (Entries.empty()) {
			return std::nullopt;
		}
		return Entries.back().Tick;
	}
}
