#include <engine/ecs/SparseSet.hpp>

namespace engine::ecs {

	SparseSet::Seat SparseSet::SeatOf(uint32_t index) {
		if (index < FIRST_PAGE_SIZE) {
			return Seat{0, index};
		}

		// Everything past the small first page is laid out in full pages, so the
		// arithmetic is the old arithmetic with the first page's slots taken off
		// the front.
		const uint32_t beyond = index - FIRST_PAGE_SIZE;
		return Seat{1 + beyond / PAGE_SIZE, beyond % PAGE_SIZE};
	}

	SparseSet::Slot &SparseSet::Reach(uint32_t index) {
		const Seat seat = SeatOf(index);
		while (Pages.size() <= seat.Page) {
			// Sized whole rather than grown, because a half-filled page would
			// have to reallocate and every location pointer handed out of it
			// would dangle.
			Pages.push_back(std::make_unique<Page>(SizeOf(Pages.size())));
		}
		return (*Pages[seat.Page])[seat.Offset];
	}

	const SparseSet::Slot *SparseSet::Peek(uint32_t index) const {
		const Seat seat = SeatOf(index);
		if (seat.Page >= Pages.size()) {
			return nullptr;
		}
		return &(*Pages[seat.Page])[seat.Offset];
	}

	SparseSet::Slot *SparseSet::Peek(uint32_t index) {
		const Seat seat = SeatOf(index);
		if (seat.Page >= Pages.size()) {
			return nullptr;
		}
		return &(*Pages[seat.Page])[seat.Offset];
	}

	uint32_t SparseSet::Allocate() {
		uint32_t index = 0;

		if (!FreeList.empty()) {
			// Last in, first out. A spawn-despawn cycle then stays inside one
			// page rather than walking the whole directory, which matters more
			// for the cache than the order does for anything else.
			index = FreeList.back();
			FreeList.pop_back();
		} else {
			index = static_cast<uint32_t>(Issued);
			Issued++;
		}

		Slot &slot = Reach(index);
		if (slot.Generation == 0) {
			slot.Generation = FIRST_GENERATION;
		}
		slot.Live = true;
		slot.Location = EntityLocation{};

		Live_++;
		return index;
	}

	void SparseSet::Free(uint32_t index) {
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

		FreeList.push_back(index);
		Live_--;
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
		Slot &slot = Reach(index);
		slot.Generation = generation == 0 ? FIRST_GENERATION : generation;
		slot.Live = live;
		slot.Location = EntityLocation{};
	}

	void SparseSet::FinishRestore(size_t issued) {
		Issued = issued;
		Live_ = 0;
		FreeList.clear();

		// Walked downwards so that the free list pops the lowest index first,
		// which keeps a restored world allocating in the same order a fresh one
		// would. Two runs that restore the same snapshot then spawn the same
		// entities agree about which index each got.
		for (size_t index = issued; index > 0; index--) {
			const Slot *slot = Peek(static_cast<uint32_t>(index - 1));
			if (slot == nullptr) {
				continue;
			}
			if (slot->Live) {
				Live_++;
			} else {
				FreeList.push_back(static_cast<uint32_t>(index - 1));
			}
		}
	}

	void SparseSet::Clear() {
		FreeList.clear();

		for (auto &page : Pages) {
			for (Slot &slot : *page) {
				if (slot.Live) {
					// Advanced rather than reset, so a handle taken before the
					// clear does not come back to life against a reissued index.
					slot.Generation++;
					if (slot.Generation == 0) {
						slot.Generation = FIRST_GENERATION;
					}
				}
				slot.Live = false;
				slot.Location = EntityLocation{};
			}
		}

		Issued = 0;
		Live_ = 0;
	}
}
