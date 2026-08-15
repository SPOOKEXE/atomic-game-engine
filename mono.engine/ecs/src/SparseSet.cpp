#include <engine/ecs/SparseSet.hpp>

#include <algorithm>

namespace engine::ecs {

	SparseSet::Seat SparseSet::SeatOf(uint32_t local) {
		if (local < FIRST_PAGE_SIZE) {
			return Seat{0, local};
		}

		// Everything past the small first page is laid out in full pages, so the
		// arithmetic is the old arithmetic with the first page's slots taken off
		// the front.
		const uint32_t beyond = local - FIRST_PAGE_SIZE;
		return Seat{1 + beyond / PAGE_SIZE, beyond % PAGE_SIZE};
	}

	SparseSet::Slot &SparseSet::Reach(uint32_t index) {
		Region &region = RegionOf(index);
		const Seat seat = SeatOf(LocalOf(index));
		while (region.Pages.size() <= seat.Page) {
			// Sized whole rather than grown, because a half-filled page would
			// have to reallocate and every location pointer handed out of it
			// would dangle.
			const size_t page = region.Pages.size();
			auto fresh = std::make_unique<Page>(SizeOf(page));

			if (page < region.Epoch.size() && region.Epoch[page] != 0) {
				// This page index has existed before and its memory went back.
				// Starting again at zero would let `Allocate` hand out
				// FIRST_GENERATION for an index whose old handles were issued
				// at exactly that, and every one of them would come alive.
				for (Slot &slot : *fresh) {
					slot.Generation = region.Epoch[page];
				}
			}

			region.Pages.push_back(std::move(fresh));
			region.LiveInPage.resize(region.Pages.size(), 0);
			if (region.Epoch.size() < region.Pages.size()) {
				region.Epoch.resize(region.Pages.size(), 0);
			}
		}
		return (*region.Pages[seat.Page])[seat.Offset];
	}

	void SparseSet::RecordEpoch(Region &region, size_t page) {
		uint32_t highest = 0;
		for (const Slot &slot : *region.Pages[page]) {
			highest = std::max(highest, slot.Generation);
		}

		// One past, so a recreated slot's generation is strictly greater than
		// anything this page ever handed out.
		region.Epoch[page] = std::max(region.Epoch[page], highest + 1);
	}

	void SparseSet::ReleaseEmptyTail(Region &region) {
		size_t keep = region.Pages.size();
		while (keep > 0 && region.LiveInPage[keep - 1] == 0) {
			keep--;
		}
		if (keep == region.Pages.size()) {
			return;
		}

		while (region.Pages.size() > keep) {
			RecordEpoch(region, region.Pages.size() - 1);
			region.Pages.pop_back();
			region.LiveInPage.pop_back();
		}

		// The released indices stop having been issued, which is what makes the
		// free-list entries naming them stale rather than a leak: `Allocate`
		// discards one when it surfaces and the fresh path hands the index out
		// again in order. Discarding eagerly here would be a pass over the whole
		// free list inside the operation whose value is being O(1).
		region.Issued = std::min(region.Issued, CoveredSlots(region));
	}

	const SparseSet::Slot *SparseSet::Peek(uint32_t index) const {
		const Region &region = RegionOf(index);
		const Seat seat = SeatOf(LocalOf(index));
		if (seat.Page >= region.Pages.size()) {
			return nullptr;
		}
		return &(*region.Pages[seat.Page])[seat.Offset];
	}

	SparseSet::Slot *SparseSet::Peek(uint32_t index) {
		Region &region = RegionOf(index);
		const Seat seat = SeatOf(LocalOf(index));
		if (seat.Page >= region.Pages.size()) {
			return nullptr;
		}
		return &(*region.Pages[seat.Page])[seat.Offset];
	}

	uint32_t SparseSet::Allocate(EntityRange range) {
		Region &region = RegionFor(range);
		const uint32_t base = range == EntityRange::Predicted ? PREDICTED_BASE : 0;
		const uint32_t owned = range == EntityRange::Predicted ? PREDICTED_INDICES : AUTHORITATIVE_INDICES;

		uint32_t index = 0;

		bool recycled = false;
		while (!region.FreeList.empty()) {
			// Last in, first out. A spawn-despawn cycle then stays inside one
			// page rather than walking the whole directory, which matters more
			// for the cache than the order does for anything else.
			//
			// Per region, so a freed predicted index comes back predicted. A
			// shared list would hand a predicted slot out as an authoritative
			// one the first time a replica recycled anything, which is the
			// collision the split exists to prevent arriving by the back door.
			const uint32_t candidate = region.FreeList.back();
			region.FreeList.pop_back();

			// An entry naming an index whose page was released is not a free
			// index any more - the high-water mark came back below it, so it is
			// one that has never been issued and the fresh path owns it. Each
			// stale entry is discarded exactly once, which is what keeps the
			// release amortised rather than a purge.
			if (static_cast<size_t>(LocalOf(candidate)) < region.Issued) {
				index = candidate;
				recycled = true;
				break;
			}
		}

		if (!recycled) {
			if (region.Issued >= owned) {
				// **Refused, never wrapped.** Handing back an index from the
				// other region would put a predicted entity on top of one the
				// authority can mint, which is the failure this whole layout
				// prevents - and it would happen at the one moment nobody is
				// looking. The caller reports it; there is no log here because
				// a directory does not know whose world it is.
				return NO_INDEX;
			}
			index = base + static_cast<uint32_t>(region.Issued);
			region.Issued++;
		}

		Slot &slot = Reach(index);
		if (slot.Generation == 0) {
			slot.Generation = FIRST_GENERATION;
		}
		slot.Live = true;
		slot.Location = EntityLocation{};

		region.LiveInPage[SeatOf(LocalOf(index)).Page]++;
		Live_++;
		return index;
	}

	void SparseSet::Free(uint32_t index) {
		// `NO_INDEX` needs no guard here: it is past every page the predicted
		// region will ever hold, so `Peek` finds nothing and the no-op below
		// covers it. `Restore` does need one, because `Reach` would allocate its
		// way there.
		Slot *slot = Peek(index);
		if (slot == nullptr || !slot->Live) {
			// Freeing twice is a no-op rather than an error, because the caller
			// above already refuses a destroy of a dead handle and a second
			// check here would only differ when that one was wrong.
			return;
		}

		slot->Live = false;
		slot->Location = EntityLocation{};

		// The bump is what invalidates every handle already issued for this
		// index. Wrapping is not defended against: at one destroy per tick per
		// entity it takes over two years at 60 Hz to wrap a 32-bit counter, and
		// the defence would cost a branch on every liveness check.
		slot->Generation++;
		if (slot->Generation == 0) {
			slot->Generation = FIRST_GENERATION;
		}

		Region &region = RegionOf(index);
		region.FreeList.push_back(index);
		Live_--;

		const size_t page = SeatOf(LocalOf(index)).Page;
		region.LiveInPage[page]--;
		if (region.LiveInPage[page] == 0) {
			// Only when a page has just emptied, so the scan below and the page
			// release it may do are paid once per page rather than per free.
			ReleaseEmptyTail(region);
		}
	}

	bool SparseSet::Alive(uint32_t index, uint32_t generation) const {
		const Slot *slot = Peek(index);
		return slot != nullptr && slot->Live && slot->Generation == generation;
	}

	bool SparseSet::Live(uint32_t index) const {
		const Slot *slot = Peek(index);
		return slot != nullptr && slot->Live;
	}

	uint32_t SparseSet::Generation(uint32_t index) const {
		const Slot *slot = Peek(index);
		return slot == nullptr ? 0 : slot->Generation;
	}

	const EntityLocation *SparseSet::Locate(uint32_t index) const {
		const Slot *slot = Peek(index);
		if (slot == nullptr || !slot->Live) {
			return nullptr;
		}
		return &slot->Location;
	}

	void SparseSet::Relocate(uint32_t index, EntityLocation location) {
		Slot *slot = Peek(index);
		if (slot == nullptr || !slot->Live) {
			return;
		}
		slot->Location = location;
	}

	void SparseSet::Restore(uint32_t index, uint32_t generation, bool live) {
		if (index == NO_INDEX) {
			// The refusal sentinel is not a slot. A snapshot naming it is
			// corrupt, and reaching for it would allocate the whole predicted
			// region's page list to store one entity nothing can address.
			return;
		}

		Slot &slot = Reach(index);
		slot.Generation = generation == 0 ? FIRST_GENERATION : generation;
		slot.Live = live;
		slot.Location = EntityLocation{};
	}

	size_t SparseSet::CoveredSlots(const Region &region) {
		size_t slots = 0;
		for (const auto &page : region.Pages) {
			slots += page->size();
		}
		return slots;
	}

	size_t SparseSet::RebuildFreeList(EntityRange range, size_t issued) {
		Region &region = RegionFor(range);
		region.Issued = issued;
		region.FreeList.clear();

		const uint32_t base = range == EntityRange::Predicted ? PREDICTED_BASE : 0;
		size_t live = 0;

		// Rebuilt here rather than maintained by `Restore`, because a bulk
		// restore is N writes and one pass by design and a per-slot counter
		// update would make it N of each.
		region.LiveInPage.assign(region.Pages.size(), 0);

		// Only as far as the pages actually reach. An index past them has never
		// been touched, so it is neither live nor free-list material and the
		// walk would only skip it - but a high-water mark restored well ahead of
		// the pages (a snapshot saying "this range is full") would make that
		// skip two billion iterations long.
		const size_t reachable = std::min(issued, CoveredSlots(region));

		// Walked downwards so that the free list pops the lowest index first,
		// which keeps a restored world allocating in the same order a fresh one
		// would. Two runs that restore the same snapshot then spawn the same
		// entities agree about which index each got.
		for (size_t local = reachable; local > 0; local--) {
			const uint32_t index = base + static_cast<uint32_t>(local - 1);
			const Slot *slot = Peek(index);
			if (slot == nullptr) {
				continue;
			}
			if (slot->Live) {
				live++;
				region.LiveInPage[SeatOf(static_cast<uint32_t>(local - 1)).Page]++;
			} else {
				region.FreeList.push_back(index);
			}
		}

		return live;
	}

	void SparseSet::FinishRestore(size_t issued, size_t predictedIssued) {
		Live_ = RebuildFreeList(EntityRange::Authoritative, issued) +
				RebuildFreeList(EntityRange::Predicted, predictedIssued);
	}

	void SparseSet::Adopt(uint32_t index, uint32_t generation) {
		if (index == NO_INDEX) {
			return;
		}

		Restore(index, generation, true);

		// Only the region the index landed in moves its high-water mark. Taking
		// the maximum against the *other* region's count is the mistake this
		// method exists to make unavailable: it would say a store had issued two
		// billion authoritative indices the moment it adopted one predicted one.
		const size_t reached = static_cast<size_t>(LocalOf(index)) + 1;
		if (IsPredicted(index)) {
			FinishRestore(Authority.Issued, std::max(Predicted.Issued, reached));
		} else {
			FinishRestore(std::max(Authority.Issued, reached), Predicted.Issued);
		}
	}

	void SparseSet::Clear() {
		for (Region *region : {&Authority, &Predicted}) {
			region->FreeList.clear();

			for (auto &page : region->Pages) {
				for (Slot &slot : *page) {
					if (slot.Live) {
						// Advanced rather than reset, so a handle taken before
						// the clear does not come back to life against a
						// reissued index.
						slot.Generation++;
						if (slot.Generation == 0) {
							slot.Generation = FIRST_GENERATION;
						}
					}
					slot.Live = false;
					slot.Location = EntityLocation{};
				}
			}

			// Every page goes back, not only the tail: a cleared world holds
			// nothing, and holding its directory would be the same leak wearing
			// the word "reuse". The epochs are what survive, so the generations
			// this clear just advanced stay spent.
			region->LiveInPage.assign(region->Pages.size(), 0);
			ReleaseEmptyTail(*region);
			region->Issued = 0;
		}

		Live_ = 0;
	}
}
