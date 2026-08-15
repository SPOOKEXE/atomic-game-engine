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
// **Paged, and a page is freed only once nothing in it is live.** A world of
// half a million entities would otherwise reallocate one contiguous block and
// copy it, at the moment it is least affordable. Pages also keep addresses
// stable, so a caller may hold a location pointer across an unrelated spawn -
// and a released page cannot take one with it, because `Locate` hands back
// nothing for a dead slot in the first place.
//
// **What a released page leaves behind is its epoch**, one word per page index
// kept for as long as the directory exists. A page's generations are what keep
// a stale handle dead, and dropping the page would drop them: a recreated page
// starting again at `FIRST_GENERATION` would bring every handle ever issued
// against it back to life. So a page records one past the highest generation it
// ever issued, and a recreated one starts every slot there. Four bytes per
// 64 KB page, and it makes the revival impossible by construction rather than
// by care.
//
// **Two regions, not one.** The index space is split in half so that a replica
// can mint an entity of its own without colliding with one the authority
// minted. See `EntityRange` and `SparseSet::PREDICTED_BASE`.
//
// @tier L3 · shared

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace engine::ecs {

	// Which half of the index space an entity was minted from.
	//
	// **Entity identity is an index plus a generation, and two independently
	// built stores both start at index 0 generation 1.** So an entity a replica
	// mints for itself collides *exactly* with one the authority minted, and a
	// merge is right to treat them as the same entity because nothing tells them
	// apart. Splitting the index space is what tells them apart: an authority
	// allocates only from the low half and a replica's prediction only from the
	// high half, so the two can never name the same slot.
	//
	// @since v0.4
	enum class EntityRange : uint8_t {
		// Minted by whoever owns the simulation. Everything a snapshot from an
		// authority mentions, and the default for every existing caller.
		Authoritative,

		// Minted by a replica for something it predicted locally, before the
		// authority has said anything about it. Promoted to an authoritative
		// identity - or dropped - once the authority answers.
		Predicted,
	};

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
		// at the scale `ecs/docs/TODO.md` actually names - a thousand small
		// worlds in one host - a thousand worlds of a hundred entities held
		// **72.7 MB resident against 2.7 MB of live rows, and 64 MB of it was
		// directory pages**, because every world was paying a 64 KB entry fee
		// for a hundred entities. Not the column capacity the TODO blamed.
		//
		// 512 slots is 8 KB. Smaller and the allocation header starts to be a
		// visible fraction of the page.
		static constexpr uint32_t FIRST_PAGE_SIZE = 512;

		// Entities per page after the first.
		//
		// Still 4096 - 64 KB - and deliberately unchanged. **Shrinking every
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
		// into its high bits is never all-zero - which is the null handle.
		// Index zero would otherwise produce a valid entity indistinguishable
		// from NULL_ENTITY.
		static constexpr uint32_t FIRST_GENERATION = 1;

		// The first index belonging to the predicted range.
		//
		// **The boundary the whole split hangs off, and it is a power of two on
		// purpose**: `index >= PREDICTED_BASE` is one compare and the
		// region-local index is one subtraction, so telling the two apart costs
		// nothing on a path every entity operation goes through.
		//
		// Halfway rather than a small reserved block at the top. Neither side
		// runs out at any population this engine will see, and a boundary that
		// is *obviously* unreachable from both directions needs no argument
		// about how many predicted entities are enough.
		static constexpr uint32_t PREDICTED_BASE = 0x8000'0000u;

		// The value a refused allocation hands back.
		//
		// Taken off the top of the predicted range rather than invented
		// somewhere else: every other `uint32_t` is a legal index in one region
		// or the other, so a sentinel has to cost somebody exactly one index.
		// The predicted side pays it, because it is the side with no snapshot
		// written before v0.4 to stay compatible with.
		static constexpr uint32_t NO_INDEX = 0xFFFF'FFFFu;

		// Indices the authoritative range holds: `[0, PREDICTED_BASE)`.
		static constexpr uint32_t AUTHORITATIVE_INDICES = PREDICTED_BASE;

		// Indices the predicted range holds: `[PREDICTED_BASE, NO_INDEX)`.
		//
		// One fewer than the authoritative range, because `NO_INDEX` is carved
		// out of the top of it.
		static constexpr uint32_t PREDICTED_INDICES = NO_INDEX - PREDICTED_BASE;

		// Reports whether an index was minted by a replica's prediction.
		//
		// @param index The index to classify.
		// @return `true` for an index in the predicted range.
		static constexpr bool IsPredicted(uint32_t index) {
			return index >= PREDICTED_BASE;
		}

		// Takes an index from one range, reusing a freed one when there is one.
		//
		// Reuse is last-in-first-out, which keeps a spawn-despawn cycle inside
		// one page instead of walking the directory. **A freed index only ever
		// comes back in the range it was minted from** - each region keeps its
		// own free list, so a predicted index cannot be recycled as an
		// authoritative one and the guarantee the split exists for holds across
		// a spawn-despawn cycle as well as a fresh mint.
		//
		// A range that has issued every index it owns **refuses** rather than
		// wrapping into the other one. Wrapping would reintroduce exactly the
		// collision the split prevents, at the one moment nobody is watching.
		//
		// @param range Which half of the index space to mint from.
		// @return The index, live and located nowhere, or `NO_INDEX` when the
		//         range is exhausted.
		uint32_t Allocate(EntityRange range = EntityRange::Authoritative);

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

		// The number of live indices, across both ranges.
		//
		// @return The live count.
		size_t LiveCount() const {
			return Live_;
		}

		// The number of authoritative indices ever handed out, live or free.
		//
		// The directory's high-water mark, which is what a caller sizing a
		// parallel array wants. **Counts the authoritative range only**, because
		// the predicted range is 2³¹ indices further up and nothing sizes an
		// array across the gap - a caller that wants both walks both.
		//
		// @return The highest authoritative index issued, plus one.
		size_t Capacity() const {
			return Authority.Issued;
		}

		// The same, for the predicted range, counted from `PREDICTED_BASE`.
		//
		// So the absolute indices a caller has to visit are
		// `PREDICTED_BASE + 0` up to `PREDICTED_BASE + PredictedCapacity()`.
		//
		// @return The highest predicted index issued, plus one, region-local.
		size_t PredictedCapacity() const {
			return Predicted.Issued;
		}

		// How many slots one range has actually allocated.
		//
		// A diagnostic, and it is here rather than inferred because the entry
		// fee this class charges a world is a property worth pinning with a
		// test rather than with a comment: a world of a hundred entities must
		// not pay for a full page, and a store that never predicts anything
		// must pay **nothing** for the predicted range.
		//
		// Slots rather than pages, because pages are the wrong unit for the
		// question. The fee is bytes, and a first page grown from 512 slots to
		// 4096 is the regression this exists to catch while leaving the page
		// *count* exactly as it was.
		//
		// @param range The range to measure.
		// @return The slots resident for it, which is zero until it is used.
		size_t ResidentSlots(EntityRange range) const {
			return CoveredSlots(RegionFor(range));
		}

		// Bytes both regions' pages are holding.
		//
		// The same fee `ResidentSlots` reports, in the unit a whole-world total
		// has to be summed in: a caller adding directory pages to column
		// capacity cannot multiply by a slot size it is not allowed to see.
		//
		// @return The resident bytes across both regions.
		size_t ResidentBytes() const {
			return (CoveredSlots(Authority) + CoveredSlots(Predicted)) * sizeof(Slot);
		}

		// Puts one index back at a generation and liveness, for a restore.
		//
		// A snapshot has to reproduce the directory exactly rather than
		// re-allocating indices in order, because a component may hold an
		// `Entity` - a hierarchy's parent, a target, an owner - and those
		// handles are only still valid if index *and* generation come back
		// unchanged.
		//
		// The index is absolute, so a predicted slot is restored as
		// `PREDICTED_BASE + local` and lands in the right region without the
		// caller saying which.
		//
		// @param index      The index to restore. `NO_INDEX` is ignored.
		// @param generation The generation it held.
		// @param live       Whether it was in use.
		void Restore(uint32_t index, uint32_t generation, bool live);

		// Rebuilds both free lists and the counters after a run of Restore
		// calls.
		//
		// Separate from Restore so that a restore of N entities is N writes and
		// one pass, rather than N insertions into a free list that is being
		// rebuilt anyway.
		//
		// @param issued          The authoritative high-water mark to restore.
		// @param predictedIssued The predicted high-water mark, region-local.
		void FinishRestore(size_t issued, size_t predictedIssued);

		// Brings one index into being at an exact generation.
		//
		// What a caller adopting a *single* handle wants - `CreateAt`, and the
		// per-entity path inside a merge - where `Restore` plus `FinishRestore`
		// is the bulk form. One call rather than two so that extending the right
		// region's high-water mark is decided here instead of at every call
		// site, which is where getting it wrong would put a predicted index in
		// the authoritative range's count.
		//
		// @param index      The absolute index to bring into being.
		// @param generation The generation it should answer at.
		void Adopt(uint32_t index, uint32_t generation);

		// Frees every index in both ranges and releases every page.
		//
		// Generations advance rather than resetting, and each page's epoch
		// outlives its memory, so a handle from before the clear does not come
		// back to life against a reissued index.
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

		// One range's pages, free list and high-water mark.
		//
		// **Two of these rather than one page list over the whole index
		// space.** The predicted range starts at 2³¹, and a single linear list
		// would have to allocate half a million pages just to reach its first
		// index. Each region pages exactly the way the single one used to, so
		// the small-first-page rule applies to both - and a store that never
		// mints a predicted entity allocates no page at all for that region,
		// because the first one is only made on the first index that needs it.
		struct Region {
			std::deque<std::unique_ptr<Page>> Pages;
			std::vector<uint32_t> FreeList;
			size_t Issued = 0;

			// Live slots per page, so a page emptying is noticed by a decrement
			// rather than by scanning it on every free.
			std::vector<uint32_t> LiveInPage;

			// One past the highest generation each page index has ever issued,
			// kept for page indices whose memory is not. See the file header:
			// this is what stops a recreated page reviving a handle.
			std::vector<uint32_t> Epoch;
		};

		// Which page a *region-local* index falls in, and where inside it.
		struct Seat {
			size_t Page = 0;
			uint32_t Offset = 0;
		};

		// Resolves a region-local index against the two page sizes.
		//
		// One place rather than four: `Reach` and both `Peek` overloads had the
		// same two lines of arithmetic, and two page sizes turn two lines into
		// four with a branch - which is three more chances for one copy to be
		// subtly different from the others.
		static Seat SeatOf(uint32_t local);

		// An absolute index as an offset inside its own region.
		static constexpr uint32_t LocalOf(uint32_t index) {
			return index >= PREDICTED_BASE ? index - PREDICTED_BASE : index;
		}

		// How many slots a page holds, which depends only on whether it is the
		// first.
		static uint32_t SizeOf(size_t page) {
			return page == 0 ? FIRST_PAGE_SIZE : PAGE_SIZE;
		}

		// The region an absolute index belongs to.
		Region &RegionOf(uint32_t index) {
			return index >= PREDICTED_BASE ? Predicted : Authority;
		}
		const Region &RegionOf(uint32_t index) const {
			return index >= PREDICTED_BASE ? Predicted : Authority;
		}

		// The region a caller named.
		Region &RegionFor(EntityRange range) {
			return range == EntityRange::Predicted ? Predicted : Authority;
		}
		const Region &RegionFor(EntityRange range) const {
			return range == EntityRange::Predicted ? Predicted : Authority;
		}

		// The slot for an index, allocating pages up to it.
		Slot &Reach(uint32_t index);

		// The slot for an index, or null when no page covers it.
		const Slot *Peek(uint32_t index) const;

		// The slot for an index, mutable, or null when no page covers it.
		Slot *Peek(uint32_t index);

		// Gives back every trailing page holding no live slot.
		//
		// Trailing only, and that is what keeps it cheap: the released indices
		// stop having been issued at all, so the high-water mark comes back down
		// with them and the free-list entries naming them are simply discarded
		// when they surface. Purging the free list eagerly is the O(FreeList)
		// pass inside `Free` that the whole directory exists to avoid.
		static void ReleaseEmptyTail(Region &region);

		// Records a page's highest generation before its memory goes back.
		static void RecordEpoch(Region &region, size_t page);

		// Rebuilds one range's free list and counts its live slots.
		//
		// @return How many of that range's slots came back live.
		size_t RebuildFreeList(EntityRange range, size_t issued);

		// How far one region's pages actually reach.
		//
		// Summed rather than derived from the page count, so it stays the truth
		// if the page sizes ever change - and so the test that pins the entry
		// fee measures the allocation rather than restating the formula.
		static size_t CoveredSlots(const Region &region);

		Region Authority;
		Region Predicted;
		size_t Live_ = 0;
	};
}
