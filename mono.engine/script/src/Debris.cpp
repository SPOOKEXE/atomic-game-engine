// The deadline queue, in insertion order underneath the tick order.
//
// Nothing here names a VM. `DebrisService.cpp` and `JsDebrisService.cpp` are
// what meet one on this file's behalf.
//
// @tier L9 · shared

#include "Debris.hpp"

#include <algorithm>

namespace engine::script {

	ecs::Entity DebrisQueue::Add(ecs::Entity instance, uint64_t dueTick) {
		// Already waiting: keep the earlier of the two deadlines and add
		// nothing — see the header for why a second entry would be dead weight.
		for (Item &item : Items) {
			if (item.Instance != instance) {
				continue;
			}
			if (dueTick < item.DueTick) {
				item.DueTick = dueTick;
				std::stable_sort(Items.begin(), Items.end(), [](const Item &left, const Item &right) {
					return left.DueTick < right.DueTick ||
						   (left.DueTick == right.DueTick && left.Sequence < right.Sequence);
				});
			}
			return ecs::NULL_ENTITY;
		}

		// At the cap, the oldest item goes now. The caller destroys it, which is
		// what the return value means — and the header says why destroying early
		// is the right way for a cleanup call to fail.
		ecs::Entity evicted = ecs::NULL_ENTITY;
		if (Items.size() >= MAXIMUM) {
			const auto oldest =
				std::min_element(Items.begin(), Items.end(), [](const Item &left, const Item &right) {
					return left.Sequence < right.Sequence;
				});
			evicted = oldest->Instance;
			Items.erase(oldest);
		}

		// Inserted in `(DueTick, Sequence)` order, so the drain is a walk from
		// the front rather than a sort per tick.
		Item item;
		item.DueTick = dueTick;
		item.Sequence = NextSequence++;
		item.Instance = instance;

		const auto at =
			std::upper_bound(Items.begin(), Items.end(), item, [](const Item &left, const Item &right) {
				return left.DueTick < right.DueTick ||
					   (left.DueTick == right.DueTick && left.Sequence < right.Sequence);
			});
		Items.insert(at, item);
		return evicted;
	}

	void DebrisQueue::Advance(uint64_t tick, std::vector<ecs::Entity> &expired) {
		// Everything at the front whose tick has arrived, in list order — which
		// is deadline order, then insertion order.
		size_t due = 0;
		while (due < Items.size() && Items[due].DueTick <= tick) {
			expired.push_back(Items[due].Instance);
			due++;
		}

		// The remainder shifts down once rather than per item.
		Items.erase(Items.begin(), Items.begin() + static_cast<std::ptrdiff_t>(due));
	}
}
