#pragma once

// arch-waiver public-header: forward ECS API. Simulation hosts keep rollback
// policy available as one complete, reusable contract.

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
	// Owns a bounded sequence of whole-store snapshots indexed by tick.
	class RollbackBuffer {
	  public:
		// Creates a history retaining at most `capacity` snapshots.
		explicit RollbackBuffer(size_t capacity);

		// Captures a snapshot at `tick`, branching away from newer history.
		bool Capture(const Store &store, uint64_t tick);
		// Replaces `store` with the snapshot at `tick` when it is retained.
		bool Restore(Store &store, uint64_t tick) const;
		// Removes every snapshot newer than `tick`.
		void DiscardAfter(uint64_t tick);

		// Reports the number of retained snapshots.
		size_t Size() const;
		// Reports the configured snapshot limit.
		size_t Capacity() const;
		// Returns the oldest retained tick, or nothing when empty.
		std::optional<uint64_t> OldestTick() const;
		// Returns the newest retained tick, or nothing when empty.
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
