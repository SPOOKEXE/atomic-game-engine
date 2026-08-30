#pragma once

// Bounded whole-world history for prediction and deterministic resimulation.
//
// Each entry is an owned snapshot. Restoring one cannot alias the live world,
// and capturing an older tick starts a new history branch by dropping entries
// at that tick and later.
//
// @tier L3 shared

#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::ecs {
	class RollbackBuffer {
	  public:
		explicit RollbackBuffer(size_t capacity);

		bool Capture(const Store &store, uint64_t tick);
		bool Restore(Store &store, uint64_t tick) const;
		void DiscardAfter(uint64_t tick);

		size_t Size() const;
		size_t Capacity() const;
		std::optional<uint64_t> OldestTick() const;
		std::optional<uint64_t> LatestTick() const;

	  private:
		struct Entry {
			uint64_t Tick = 0;
			std::vector<std::byte> Snapshot;
		};

		size_t EntryCapacity = 0;
		std::vector<Entry> Entries;
	};
}
