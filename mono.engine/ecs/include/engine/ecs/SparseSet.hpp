#pragma once

// The entity directory: which archetype and row an entity lives in, and whether
// it is still the entity somebody is holding a handle to.
//
// Every entity operation goes through here first. `Alive` is a generation
// compare, `Get` is two array lookups, and destroying an entity is a swap-back
// in a column plus one write here. That is the whole reason an entity handle
// can be a bare integer with no pointer in it.
//
// **Generations are what make a stale handle safe.** An index is recycled as
// soon as the entity using it is destroyed, because leaving holes would make
// the directory grow forever in a world that spawns and despawns. Recycling
// alone would mean an old handle silently naming a new entity, so every reuse
// bumps a counter, and a handle carries the counter it was issued with. The
// pair is what identifies an entity; the index alone never does.
//
// **Paged, and pages are never freed.** A world of half a million entities
// would otherwise reallocate one contiguous block and copy it, at the moment it
// is least affordable. Pages also keep addresses stable, so a caller may hold a
// location pointer across an unrelated spawn.
//
// @tier L3 · shared

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace engine::ecs {

	// Where one entity's components live.
	//
	// An archetype index rather than a pointer, so the directory survives the
	// archetype table growing and so nothing here has to know what an archetype
	// is.
	//
	// @since v0.2
	struct EntityLocation {
		// The value meaning "this entity is in no archetype".
		static constexpr uint32_t NO_ARCHETYPE = 0xFFFFFFFFu;

		// The archetype holding this entity's row.
		uint32_t Archetype = NO_ARCHETYPE;

		// The row within that archetype's columns.
		//
		// Not stable: removing any row in the archetype moves its last row into
		// the hole, and whichever entity that was has its location rewritten.
		uint32_t Row = 0;
	};

	// A paged index from entity index to generation and location.
	//
	// @since v0.2
	class SparseSet {
	  public:
		// Entities in the first page.
		//
		// **A world's entry fee for having a directory at all.** A page is
		// allocated whole, and the first one is allocated on the first entity,
		// so every world pays this much whether it holds ten entities or the
		// page's worth. That went unnoticed while there was one world: measured
		// at the scale `ecs/docs/TODO.md` actually names — a thousand small
		// worlds in one host — a thousand worlds of a hundred entities held
		// **72.7 MB resident against 2.7 MB of live rows, and 64 MB of it was
		// directory pages**, because every world was paying a 64 KB entry fee
		// for a hundred entities. Not the column capacity the TODO blamed.
		//
		// 512 slots is 8 KB. Smaller and the allocation header starts to be a
		// visible fraction of the page.
		static constexpr uint32_t FIRST_PAGE_SIZE = 512;

		// Entities per page after the first.
		//
		// Still 4096 — 64 KB — and deliberately unchanged. **Shrinking every
		// page rather than only the first was measured and rejected**: it costs
		// a large world eight times as many allocations, which interleave with
		// the columns and scatter them, and a multi-world tick over 100k
		// entities each came out 8% to 21% slower across repeated runs. Two
		// sizes rather than one keeps a big world's heap exactly as it was while
		// a small world stops paying for a page it will never fill.
		//
		// The cost is one branch in the index arithmetic, which does not show
		// against a directory lookup that misses cache.
		static constexpr uint32_t PAGE_SIZE = 4096;

		// The generation a never-used index starts at.
		//
		// One rather than zero, so that an entity handle packing generation
		// into its high bits is never all-zero — which is the null handle.
		// Index zero would otherwise produce a valid entity indistinguishable
		// from NULL_ENTITY.
		static constexpr uint32_t FIRST_GENERATION = 1;

		// Takes an index, reusing a freed one when there is one.
		//
		// Reuse is last-in-first-out, which keeps a spawn-despawn cycle inside
		// one page instead of walking the directory.
		//
		// @return The index, live and located nowhere.
		uint32_t Allocate();

		// Returns an index to the free list and bumps its generation.
		//
		// Every handle holding the old generation stops being alive at this
		// point, which is what makes a use-after-destroy a failed check rather
		// than a read of somebody else's components.
		//
		// @param index The index to free. Ignored when not live.
		void Free(uint32_t index);

		// Reports whether an index is live at a generation.
		//
		// @param index      The index to check.
		// @param generation The generation the handle was issued with.
		// @return `true` when the index is live and generations match.
		bool Alive(uint32_t index, uint32_t generation) const;

		// Reports whether an index is live at any generation.
		//
		// @param index The index to check.
		// @return `true` when the index is in use.
		bool Live(uint32_t index) const;

		// The current generation of an index.
		//
		// @param index The index to inspect.
		// @return The generation, or zero for an index that has never existed.
		uint32_t Generation(uint32_t index) const;

		// Where a live index's row is.
		//
		// @param index The index to locate.
		// @return The location, or `nullptr` when the index is not live.
		const EntityLocation *Locate(uint32_t index) const;

		// Records where a live index's row is.
		//
		// @param index    The index to relocate. Ignored when not live.
		// @param location The archetype and row now holding it.
		void Relocate(uint32_t index, EntityLocation location);

		// The number of live indices.
		//
		// @return The live count.
		size_t LiveCount() const {
			return Live_;
		}

		// The number of indices ever handed out, live or free.
		//
		// The directory's high-water mark, which is what a caller sizing a
		// parallel array wants.
		//
		// @return The highest index issued, plus one.
		size_t Capacity() const {
			return Issued;
		}

		// Puts one index back at a generation and liveness, for a restore.
		//
		// A snapshot has to reproduce the directory exactly rather than
		// re-allocating indices in order, because a component may hold an
		// `Entity` — a hierarchy's parent, a target, an owner — and those
		// handles are only still valid if index *and* generation come back
		// unchanged.
		//
		// @param index      The index to restore.
		// @param generation The generation it held.
		// @param live       Whether it was in use.
		void Restore(uint32_t index, uint32_t generation, bool live);

		// Rebuilds the free list and counters after a run of Restore calls.
		//
		// Separate from Restore so that a restore of N entities is N writes and
		// one pass, rather than N insertions into a free list that is being
		// rebuilt anyway.
		//
		// @param issued The high-water mark to restore.
		void FinishRestore(size_t issued);

		// Frees every index and keeps the pages.
		//
		// Generations advance rather than resetting, so a handle from before
		// the clear does not come back to life.
		void Clear();

	  private:
		struct Slot {
			uint32_t Generation = 0;
			bool Live = false;
			EntityLocation Location;
		};

		// One page of slots. Held by pointer so that a page address is stable
		// once allocated.
		using Page = std::vector<Slot>;

		// Which page an index falls in, and where inside it.
		struct Seat {
			size_t Page = 0;
			uint32_t Offset = 0;
		};

		// Resolves an index against the two page sizes.
		//
		// One place rather than four: `Reach` and both `Peek` overloads had the
		// same two lines of arithmetic, and two page sizes turn two lines into
		// four with a branch — which is three more chances for one copy to be
		// subtly different from the others.
		static Seat SeatOf(uint32_t index);

		// How many slots a page holds, which depends only on whether it is the
		// first.
		static uint32_t SizeOf(size_t page) {
			return page == 0 ? FIRST_PAGE_SIZE : PAGE_SIZE;
		}

		// The slot for an index, allocating pages up to it.
		Slot &Reach(uint32_t index);

		// The slot for an index, or null when no page covers it.
		const Slot *Peek(uint32_t index) const;

		// The slot for an index, mutable, or null when no page covers it.
		Slot *Peek(uint32_t index);

		std::deque<std::unique_ptr<Page>> Pages;
		std::vector<uint32_t> FreeList;
		size_t Issued = 0;
		size_t Live_ = 0;
	};
}
