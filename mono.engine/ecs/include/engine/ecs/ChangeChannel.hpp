#pragma once

// arch-waiver public-header: forward ECS API. Systems use this complete change
// stream contract to observe store mutations without a second event model.

// Which components changed this tick, per entity.
//
// Three things need the same answer and would otherwise each invent it:
//
// - **`.Changed`** in the instance model, where a script wants to know that
//   *this* part moved rather than that *some* part did,
// - **the render graph**, which caches per node and has to invalidate exactly
//   the nodes whose inputs moved,
// - **replication**, where a delta is precisely the set of rows that changed
//   since the client was last told.
//
// **The bits are a column, not a side table.** `DirtyBits` sits in the
// archetype beside the columns it describes, so a snapshot carries it, a query
// finds it contiguous, and it exists only in the archetypes of components
// somebody actually observes. One bit per component per row: 64 rows' worth of
// tracking costs 512 bytes.
//
// **It is not a tag, and that distinction is the whole design.** A tag is part
// of the `ComponentSet`, so setting one would move the row to a different
// archetype - a structural change on every property write, and archetype churn
// every tick. A bitmask column is a plain write to memory the row already owns.
// The difference between the two is the difference between free and unusable.
//
// **What it cannot see.** `EachBatch` and `EachBatchParallel` hand out raw
// column pointers, and a write through one sets no bit. That is deliberate -
// checking per row is exactly the cost those paths exist to avoid - and it is
// the gap a content signature fills for a consumer that needs row granularity
// over batch-written data - `gui::Compiled` and `studio::HierarchyView` are the
// two that do, and each folds its own hash rather than sharing one. A batch write does bump the coarse
// version, so a consumer that only needs "did anything move" is still served.
//
// @tier L3 · shared

#include <engine/ecs/TypeDescriptor.hpp>

#include <cstdint>

namespace engine::ecs {

	// One row's changed-component bits, one bit per column position.
	//
	// The bit index is the component's position in its archetype's sorted set,
	// which is the same position the column sits at - so marking a change is an
	// index the store already has rather than a lookup.
	//
	// @since v0.2
	struct DirtyBits {
		// The maximum number of components one archetype can track.
		//
		// Sixty-four, because that is what fits in the mask. An archetype wider
		// than this cannot be observed, and the store says so rather than
		// silently tracking the first sixty-four.
		static constexpr size_t CAPACITY = 64;

		// One bit per column position, set when that component was written.
		uint64_t Mask = 0;

		// Reports whether one position was written since the last clear.
		//
		// @param position The column position to test.
		// @return `true` when that component changed.
		constexpr bool Test(size_t position) const {
			return position < CAPACITY && (Mask & (uint64_t{1} << position)) != 0;
		}

		// Records that one position was written.
		//
		// @param position The column position to mark.
		constexpr void Mark(size_t position) {
			if (position < CAPACITY) {
				Mask |= uint64_t{1} << position;
			}
		}

		// Reports whether nothing changed.
		//
		// @return `true` when no bit is set.
		constexpr bool Quiet() const {
			return Mask == 0;
		}
	};
}
