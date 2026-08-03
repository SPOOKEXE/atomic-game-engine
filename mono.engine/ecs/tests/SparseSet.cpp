#include <engine/core/Random.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.ecs.sparseset")

using engine::core::Random;
using engine::ecs::EntityLocation;
using engine::ecs::EntityRange;
using engine::ecs::SparseSet;

TEST_CASE("a fresh directory is empty", "[ecs]") {
	SparseSet set;
	REQUIRE(set.LiveCount() == 0);
	REQUIRE(set.Capacity() == 0);
	REQUIRE_FALSE(set.Live(0));
	REQUIRE(set.Locate(0) == nullptr);
	REQUIRE(set.Generation(0) == 0);
}

TEST_CASE("allocation counts up from zero", "[ecs]") {
	SparseSet set;

	REQUIRE(set.Allocate() == 0);
	REQUIRE(set.Allocate() == 1);
	REQUIRE(set.Allocate() == 2);

	REQUIRE(set.LiveCount() == 3);
	REQUIRE(set.Capacity() == 3);
}

TEST_CASE("a fresh index starts at the first generation", "[ecs]") {
	// Never zero, because an entity handle packs the generation into its high
	// bits and index zero at generation zero would be indistinguishable from
	// the null handle.
	SparseSet set;
	const uint32_t index = set.Allocate();

	REQUIRE(index == 0);
	REQUIRE(set.Generation(index) == SparseSet::FIRST_GENERATION);
	REQUIRE(set.Generation(index) != 0);
}

TEST_CASE("freeing invalidates every handle at the old generation", "[ecs]") {
	SparseSet set;
	const uint32_t index = set.Allocate();
	const uint32_t generation = set.Generation(index);

	REQUIRE(set.Alive(index, generation));

	set.Free(index);

	REQUIRE_FALSE(set.Alive(index, generation));
	REQUIRE_FALSE(set.Live(index));
	REQUIRE(set.LiveCount() == 0);
	REQUIRE(set.Locate(index) == nullptr);
}

TEST_CASE("a recycled index does not answer to the old generation", "[ecs]") {
	// The whole reason generations exist: without the bump, a handle to a
	// destroyed entity would silently name whatever spawned into its slot.
	SparseSet set;

	const uint32_t first = set.Allocate();
	const uint32_t stale = set.Generation(first);
	set.Free(first);

	const uint32_t second = set.Allocate();
	REQUIRE(second == first); // the index was reused
	REQUIRE(set.Generation(second) != stale);

	REQUIRE(set.Alive(second, set.Generation(second)));
	REQUIRE_FALSE(set.Alive(first, stale));
}

TEST_CASE("reuse is last in, first out", "[ecs]") {
	SparseSet set;
	for (int index = 0; index < 4; index++) {
		set.Allocate();
	}

	set.Free(1);
	set.Free(3);

	// A spawn-despawn cycle staying in one page is worth more than any
	// ordering property here, and LIFO is what keeps it there.
	REQUIRE(set.Allocate() == 3);
	REQUIRE(set.Allocate() == 1);
	REQUIRE(set.Allocate() == 4); // free list exhausted, so a new index
}

TEST_CASE("freeing twice is a no-op", "[ecs]") {
	SparseSet set;
	const uint32_t index = set.Allocate();

	set.Free(index);
	set.Free(index);

	// One entry on the free list, not two — otherwise the next two allocations
	// would hand the same index out twice.
	REQUIRE(set.LiveCount() == 0);
	const uint32_t first = set.Allocate();
	const uint32_t second = set.Allocate();
	REQUIRE(first != second);
}

TEST_CASE("freeing an index that never existed is a no-op", "[ecs]") {
	SparseSet set;
	set.Free(0);
	set.Free(1'000'000);

	REQUIRE(set.LiveCount() == 0);
	REQUIRE(set.Allocate() == 0);
}

TEST_CASE("location round-trips", "[ecs]") {
	SparseSet set;
	const uint32_t index = set.Allocate();

	// A fresh index is live but in no archetype, which is what an entity looks
	// like between being created and being given its first component.
	REQUIRE(set.Locate(index)->Archetype == EntityLocation::NO_ARCHETYPE);

	set.Relocate(index, EntityLocation{7, 42});

	REQUIRE(set.Locate(index)->Archetype == 7);
	REQUIRE(set.Locate(index)->Row == 42);
}

TEST_CASE("relocating a dead index does nothing", "[ecs]") {
	SparseSet set;
	const uint32_t index = set.Allocate();
	set.Free(index);

	set.Relocate(index, EntityLocation{1, 1});
	REQUIRE(set.Locate(index) == nullptr);
}

TEST_CASE("a reallocated index forgets where it was", "[ecs]") {
	// A recycled slot carrying the previous entity's location would put a new
	// entity's components at somebody else's row.
	SparseSet set;

	const uint32_t first = set.Allocate();
	set.Relocate(first, EntityLocation{3, 9});
	set.Free(first);

	const uint32_t second = set.Allocate();
	REQUIRE(second == first);
	REQUIRE(set.Locate(second)->Archetype == EntityLocation::NO_ARCHETYPE);
	REQUIRE(set.Locate(second)->Row == 0);
}

TEST_CASE("indices past the first page work", "[ecs]") {
	SparseSet set;

	// Three pages' worth, so the paging maths is exercised at both boundaries
	// rather than only within the first block. The first page is deliberately
	// smaller than the rest, so the two boundaries are at different strides —
	// which is exactly the arithmetic worth testing.
	const uint32_t count = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE * 2 + 5;
	for (uint32_t index = 0; index < count; index++) {
		REQUIRE(set.Allocate() == index);
	}

	REQUIRE(set.LiveCount() == count);

	set.Relocate(SparseSet::FIRST_PAGE_SIZE - 1, EntityLocation{1, 1});
	set.Relocate(SparseSet::FIRST_PAGE_SIZE, EntityLocation{2, 2});
	set.Relocate(SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE - 1, EntityLocation{3, 3});
	set.Relocate(SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE, EntityLocation{4, 4});

	REQUIRE(set.Locate(SparseSet::FIRST_PAGE_SIZE - 1)->Archetype == 1);
	REQUIRE(set.Locate(SparseSet::FIRST_PAGE_SIZE)->Archetype == 2);
	REQUIRE(set.Locate(SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE - 1)->Archetype == 3);
	REQUIRE(set.Locate(SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE)->Archetype == 4);
}

TEST_CASE("every index in the first three pages is its own slot", "[ecs]") {
	// Two page sizes make the index arithmetic a branch plus a subtraction, and
	// an off-by-one there aliases two indices onto one slot — which shows up as
	// two entities sharing a location rather than as a crash. Writing a distinct
	// value through every index and reading them all back catches that
	// wherever it is, rather than only at the boundaries somebody thought of.
	SparseSet set;

	const uint32_t count = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE * 2;
	for (uint32_t index = 0; index < count; index++) {
		REQUIRE(set.Allocate() == index);
		set.Relocate(index, EntityLocation{index, index + 1});
	}

	for (uint32_t index = 0; index < count; index++) {
		const EntityLocation *location = set.Locate(index);
		REQUIRE(location != nullptr);
		REQUIRE(location->Archetype == index);
		REQUIRE(location->Row == index + 1);
	}
}

TEST_CASE("a location pointer survives an unrelated allocation", "[ecs]") {
	// Pages are what buy this. A contiguous vector would reallocate on growth
	// and leave the pointer dangling, and the caller reading through it would
	// see whatever the allocator did next.
	SparseSet set;
	const uint32_t held = set.Allocate();
	set.Relocate(held, EntityLocation{5, 5});

	const EntityLocation *location = set.Locate(held);
	REQUIRE(location != nullptr);

	for (uint32_t index = 0; index < SparseSet::PAGE_SIZE * 3; index++) {
		set.Allocate();
	}

	REQUIRE(location->Archetype == 5);
	REQUIRE(location->Row == 5);
	REQUIRE(location == set.Locate(held));
}

TEST_CASE("clear frees everything without reviving old handles", "[ecs]") {
	SparseSet set;

	std::vector<std::pair<uint32_t, uint32_t>> handles;
	for (int index = 0; index < 8; index++) {
		const uint32_t allocated = set.Allocate();
		handles.emplace_back(allocated, set.Generation(allocated));
	}

	set.Clear();
	REQUIRE(set.LiveCount() == 0);
	REQUIRE(set.Capacity() == 0);

	// The index counter restarts, so the same indices come back — and every
	// handle taken before the clear must still be dead against them.
	const uint32_t reissued = set.Allocate();
	REQUIRE(reissued == 0);

	size_t revived = 0;
	for (const auto &[index, generation] : handles) {
		if (set.Alive(index, generation)) {
			revived++;
		}
	}
	REQUIRE(revived == 0);
}

// --- the two index regions -------------------------------------------------
//
// The index space is split so a replica can mint an entity of its own without
// colliding with one the authority minted. Everything below pins one half of
// that: that the two ranges cannot reach each other, and that the second region
// costs a world nothing until it uses one.

TEST_CASE("the two ranges divide the whole index space and do not overlap", "[ecs]") {
	// The arithmetic, written down once where a change to it fails rather than
	// where it is merely described. `NO_INDEX` is carved out of the top of the
	// predicted range, which is why that side is one index short.
	STATIC_REQUIRE(SparseSet::PREDICTED_BASE == 0x8000'0000u);
	STATIC_REQUIRE(SparseSet::AUTHORITATIVE_INDICES == SparseSet::PREDICTED_BASE);
	STATIC_REQUIRE(SparseSet::PREDICTED_INDICES == SparseSet::NO_INDEX - SparseSet::PREDICTED_BASE);
	STATIC_REQUIRE(
		static_cast<uint64_t>(SparseSet::AUTHORITATIVE_INDICES) + SparseSet::PREDICTED_INDICES + 1 ==
		0x1'0000'0000ull
	);

	STATIC_REQUIRE_FALSE(SparseSet::IsPredicted(0));
	STATIC_REQUIRE_FALSE(SparseSet::IsPredicted(SparseSet::PREDICTED_BASE - 1));
	STATIC_REQUIRE(SparseSet::IsPredicted(SparseSet::PREDICTED_BASE));
	STATIC_REQUIRE(SparseSet::IsPredicted(SparseSet::NO_INDEX));
}

TEST_CASE("the default range never hands out a predicted index", "[ecs]") {
	// The property the whole split rests on, over enough churn that a
	// wrap-around or a shared free list would have shown. Recycles included:
	// getting the fresh path right and the reuse path wrong is the failure that
	// only appears in a world that has been running a while.
	SparseSet set;
	std::vector<uint32_t> live;

	size_t predicted = 0;
	for (uint32_t step = 0; step < 50'000; step++) {
		if (!live.empty() && (Random::Bits(step, 7) % 3) == 0) {
			const size_t at = Random::Bits(step, 8) % live.size();
			set.Free(live[at]);
			live[at] = live.back();
			live.pop_back();
			continue;
		}

		const uint32_t index = set.Allocate();
		if (SparseSet::IsPredicted(index)) {
			predicted++;
		}
		live.push_back(index);
	}

	REQUIRE(predicted == 0);
	REQUIRE(set.PredictedCapacity() == 0);
	REQUIRE(set.ResidentSlots(EntityRange::Predicted) == 0);
}

TEST_CASE("a predicted index and an authoritative one are never the same slot", "[ecs]") {
	SparseSet set;

	const uint32_t mine = set.Allocate(EntityRange::Predicted);
	const uint32_t theirs = set.Allocate(EntityRange::Authoritative);

	REQUIRE(mine == SparseSet::PREDICTED_BASE);
	REQUIRE(theirs == 0);
	REQUIRE(mine != theirs);

	// Distinct slots, not merely distinct numbers: writing through one must not
	// be visible through the other.
	set.Relocate(mine, EntityLocation{11, 12});
	set.Relocate(theirs, EntityLocation{21, 22});

	REQUIRE(set.Locate(mine)->Archetype == 11);
	REQUIRE(set.Locate(theirs)->Archetype == 21);
	REQUIRE(set.LiveCount() == 2);
	REQUIRE(set.Capacity() == 1);
	REQUIRE(set.PredictedCapacity() == 1);
}

TEST_CASE("a freed predicted index comes back predicted", "[ecs]") {
	// Two free lists rather than one. A shared list would hand a predicted slot
	// out as the next authoritative allocation, which is the collision the split
	// exists to prevent arriving by the back door.
	SparseSet set;

	// A third that stays live, so the region's page is not released underneath
	// the reuse this case is about — a region with nothing live in it gives its
	// page back and its free list stops naming anything, which the case below
	// covers.
	const uint32_t held = set.Allocate(EntityRange::Predicted);
	const uint32_t first = set.Allocate(EntityRange::Predicted);
	const uint32_t second = set.Allocate(EntityRange::Predicted);
	set.Free(first);
	set.Free(second);

	// The authoritative side must not see either of them.
	REQUIRE(set.Allocate(EntityRange::Authoritative) == 0);

	REQUIRE(set.Allocate(EntityRange::Predicted) == second);
	REQUIRE(set.Allocate(EntityRange::Predicted) == first);
	REQUIRE(SparseSet::IsPredicted(set.Allocate(EntityRange::Predicted)));
	REQUIRE(set.Live(held));
}

TEST_CASE("a region that empties gives its pages back and still keeps handles dead", "[ecs]") {
	// **The directory's own leftover**, and the hazard that comes with taking
	// it. A world that grew to a hundred thousand entities and dropped back to a
	// hundred used to hold every page it ever touched — measured at 204 KB of
	// directory for a hundred live entities, which is most of what a settled
	// world costs once the columns stop holding their peak.
	//
	// Releasing a page drops the generations in it, and that is the whole risk:
	// a recreated page starting again at `FIRST_GENERATION` hands a reissued
	// index back at exactly the generation the oldest handles were issued with,
	// and every one of them comes alive. The page's epoch is what makes that
	// impossible.
	SparseSet set;

	std::vector<std::pair<uint32_t, uint32_t>> handles;
	const uint32_t count = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE + 100;
	for (uint32_t index = 0; index < count; index++) {
		const uint32_t allocated = set.Allocate();
		handles.emplace_back(allocated, set.Generation(allocated));
	}
	REQUIRE(
		set.ResidentSlots(EntityRange::Authoritative) == SparseSet::FIRST_PAGE_SIZE + 2 * SparseSet::PAGE_SIZE
	);

	// Freed from the top, which is the shape a world settling from a peak has.
	for (uint32_t index = count; index > 100; index--) {
		set.Free(index - 1);
	}

	// Only the page the survivors are in is still resident.
	REQUIRE(set.LiveCount() == 100);
	REQUIRE(set.ResidentSlots(EntityRange::Authoritative) == SparseSet::FIRST_PAGE_SIZE);
	REQUIRE(set.Capacity() == SparseSet::FIRST_PAGE_SIZE);

	// Growing back reissues the same indices, and not one pre-release handle may
	// come alive against them.
	for (uint32_t index = 0; index < count; index++) {
		set.Allocate();
	}

	size_t revived = 0;
	for (const auto &[index, generation] : handles) {
		if (index >= 100 && set.Alive(index, generation)) {
			revived++;
		}
	}
	REQUIRE(revived == 0);
}

TEST_CASE("a page still holding one live index is kept", "[ecs]") {
	// Trailing pages only. A page with anything live in it holds locations
	// somebody may be pointing at, and releasing it would be a use-after-free
	// rather than a lost generation.
	SparseSet set;
	const uint32_t count = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE;
	for (uint32_t index = 0; index < count; index++) {
		set.Allocate();
	}

	const uint32_t survivor = count - 1;
	set.Relocate(survivor, EntityLocation{7, 9});
	const EntityLocation *location = set.Locate(survivor);

	for (uint32_t index = 0; index + 1 < count; index++) {
		set.Free(index);
	}

	REQUIRE(set.ResidentSlots(EntityRange::Authoritative) == count);
	REQUIRE(location == set.Locate(survivor));
	REQUIRE(location->Archetype == 7);
}

TEST_CASE("a store that predicts nothing pays nothing for the predicted region", "[ecs]") {
	// **The entry fee, per region.** A page is allocated whole on the first
	// index that needs it, so a second region that is never used must allocate
	// no page at all — otherwise every world in a thousand-world host would pay
	// the predicted range's entry fee for a feature only a client uses.
	SparseSet set;
	for (uint32_t index = 0; index < 100; index++) {
		set.Allocate();
	}

	REQUIRE(set.ResidentSlots(EntityRange::Predicted) == 0);

	// And the moment one is minted, it costs exactly one page, and that page is
	// the small one — 8 KB rather than the 64 KB entry fee the first-page rule
	// exists to have removed.
	set.Allocate(EntityRange::Predicted);
	REQUIRE(set.ResidentSlots(EntityRange::Predicted) == SparseSet::FIRST_PAGE_SIZE);
}

TEST_CASE("the small first page survives in both regions", "[ecs]") {
	// The measurement behind `FIRST_PAGE_SIZE` — 72.7 MB down to 16.7 MB across
	// a thousand small worlds — is a property of the first page being 512 slots
	// and not 4096. A second region must not reintroduce the fee it removed, so
	// this pins the boundary on both sides: the first page ends at
	// FIRST_PAGE_SIZE, and the one after it is a full page.
	SparseSet authority;
	authority.Allocate();
	REQUIRE(authority.ResidentSlots(EntityRange::Authoritative) == SparseSet::FIRST_PAGE_SIZE);
	for (uint32_t index = 1; index < SparseSet::FIRST_PAGE_SIZE; index++) {
		authority.Allocate();
	}
	REQUIRE(authority.ResidentSlots(EntityRange::Authoritative) == SparseSet::FIRST_PAGE_SIZE);
	authority.Allocate();
	REQUIRE(
		authority.ResidentSlots(EntityRange::Authoritative) ==
		SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE
	);

	SparseSet predicted;
	predicted.Allocate(EntityRange::Predicted);
	REQUIRE(predicted.ResidentSlots(EntityRange::Predicted) == SparseSet::FIRST_PAGE_SIZE);
	REQUIRE(predicted.ResidentSlots(EntityRange::Authoritative) == 0);
	for (uint32_t index = 1; index < SparseSet::FIRST_PAGE_SIZE; index++) {
		predicted.Allocate(EntityRange::Predicted);
	}
	REQUIRE(predicted.ResidentSlots(EntityRange::Predicted) == SparseSet::FIRST_PAGE_SIZE);
	predicted.Allocate(EntityRange::Predicted);
	REQUIRE(
		predicted.ResidentSlots(EntityRange::Predicted) == SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE
	);
}

TEST_CASE("every index across a predicted page boundary is its own slot", "[ecs]") {
	// The same aliasing check the authoritative region gets, because the
	// predicted region's arithmetic is the authoritative one plus a subtraction
	// — and an off-by-one in that subtraction puts two entities in one slot
	// rather than crashing.
	SparseSet set;

	const uint32_t count = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE + 5;
	for (uint32_t local = 0; local < count; local++) {
		const uint32_t index = set.Allocate(EntityRange::Predicted);
		REQUIRE(index == SparseSet::PREDICTED_BASE + local);
		set.Relocate(index, EntityLocation{local, local + 1});
	}

	for (uint32_t local = 0; local < count; local++) {
		const EntityLocation *location = set.Locate(SparseSet::PREDICTED_BASE + local);
		REQUIRE(location != nullptr);
		REQUIRE(location->Archetype == local);
		REQUIRE(location->Row == local + 1);
	}
}

TEST_CASE("an exhausted range refuses rather than wrapping into the other", "[ecs]") {
	// Wrapping would put a predicted entity on top of an index the authority can
	// mint — the exact collision the split exists to prevent — at the one moment
	// nobody is watching. So exhaustion is a refusal, and it says which value
	// means refused.
	//
	// Driven through the restore path rather than by allocating two billion
	// indices: a high-water mark is what `Allocate` consults, and a snapshot
	// claiming a full range is a legitimate way to set one.
	SparseSet full;

	// Timed, and the only timed assertion here. **`FinishRestore` has to cost
	// what the directory holds, not what it claims** — it rebuilds a free list
	// by walking the range, and walking two billion slots that no page covers
	// takes tens of seconds. There is no *wrong answer* to catch: the walk skips
	// the missing slots either way, so cost is the whole difference and a clock
	// is the only thing that can see it. The budget is four orders of magnitude
	// above the real figure and one below the unbounded one, so it is a deadline
	// nothing but that regression can miss.
	const auto started = std::chrono::steady_clock::now();
	full.FinishRestore(SparseSet::AUTHORITATIVE_INDICES, SparseSet::PREDICTED_INDICES);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);

	REQUIRE(full.Capacity() == SparseSet::AUTHORITATIVE_INDICES);
	REQUIRE(full.PredictedCapacity() == SparseSet::PREDICTED_INDICES);
	REQUIRE(full.Allocate(EntityRange::Authoritative) == SparseSet::NO_INDEX);
	REQUIRE(full.Allocate(EntityRange::Predicted) == SparseSet::NO_INDEX);

	// One side being full says nothing about the other, which is the point of
	// keeping the counters apart.
	SparseSet halfway;
	halfway.FinishRestore(SparseSet::AUTHORITATIVE_INDICES, 0);
	REQUIRE(halfway.Allocate(EntityRange::Authoritative) == SparseSet::NO_INDEX);
	REQUIRE(halfway.Allocate(EntityRange::Predicted) == SparseSet::PREDICTED_BASE);
}

TEST_CASE("a restore reproduces both regions exactly", "[ecs]") {
	SparseSet source;
	const uint32_t kept = source.Allocate();
	const uint32_t dropped = source.Allocate();
	const uint32_t guess = source.Allocate(EntityRange::Predicted);
	source.Free(dropped);

	SparseSet restored;
	for (uint32_t index = 0; index < source.Capacity(); index++) {
		restored.Restore(index, source.Generation(index), source.Live(index));
	}
	for (uint32_t local = 0; local < source.PredictedCapacity(); local++) {
		const uint32_t index = SparseSet::PREDICTED_BASE + local;
		restored.Restore(index, source.Generation(index), source.Live(index));
	}
	restored.FinishRestore(source.Capacity(), source.PredictedCapacity());

	REQUIRE(restored.Capacity() == source.Capacity());
	REQUIRE(restored.PredictedCapacity() == source.PredictedCapacity());
	REQUIRE(restored.LiveCount() == source.LiveCount());
	REQUIRE(restored.Alive(kept, source.Generation(kept)));
	REQUIRE(restored.Alive(guess, source.Generation(guess)));
	REQUIRE_FALSE(restored.Live(dropped));

	// The freed slot goes back on its own region's list, so the next mint on
	// each side lands where it did before.
	REQUIRE(restored.Allocate(EntityRange::Authoritative) == dropped);
	REQUIRE(restored.Allocate(EntityRange::Predicted) == SparseSet::PREDICTED_BASE + 1);
}

TEST_CASE("adopting one index only moves its own region's high-water mark", "[ecs]") {
	// The mistake `Adopt` exists to make unavailable: taking the maximum against
	// the wrong region would say a store had issued two billion authoritative
	// indices the moment it adopted one predicted one.
	SparseSet set;
	set.Adopt(SparseSet::PREDICTED_BASE + 4, 3);

	REQUIRE(set.Capacity() == 0);
	REQUIRE(set.PredictedCapacity() == 5);
	REQUIRE(set.Alive(SparseSet::PREDICTED_BASE + 4, 3));

	set.Adopt(9, 2);
	REQUIRE(set.Capacity() == 10);
	REQUIRE(set.PredictedCapacity() == 5);
	REQUIRE(set.Alive(9, 2));
	REQUIRE(set.LiveCount() == 2);
}

TEST_CASE("clear empties both regions", "[ecs]") {
	SparseSet set;
	const uint32_t theirs = set.Allocate();
	const uint32_t mine = set.Allocate(EntityRange::Predicted);
	const uint32_t was = set.Generation(mine);

	set.Clear();

	REQUIRE(set.LiveCount() == 0);
	REQUIRE(set.Capacity() == 0);
	REQUIRE(set.PredictedCapacity() == 0);
	REQUIRE_FALSE(set.Live(theirs));
	REQUIRE_FALSE(set.Alive(mine, was));

	// Reissued from the base of each region, and the old handle stays dead.
	REQUIRE(set.Allocate(EntityRange::Predicted) == mine);
	REQUIRE_FALSE(set.Alive(mine, was));
}

TEST_CASE("live indices are never handed out twice", "[ecs]") {
	// The property everything else rests on. A random interleaving of allocate
	// and free, checked against a plain model — `core::Random` so a failure
	// reproduces from the seed on any machine.
	SparseSet set;
	std::set<uint32_t> live;
	std::unordered_map<uint32_t, uint32_t> generations;

	constexpr uint32_t STEPS = 20'000;
	size_t duplicates = 0;
	size_t staleAlive = 0;

	std::vector<std::pair<uint32_t, uint32_t>> retired;

	for (uint32_t step = 0; step < STEPS; step++) {
		const bool freeing = !live.empty() && (Random::Bits(step, 1) % 3) == 0;

		if (freeing) {
			// Free an arbitrary live index.
			auto iterator = live.begin();
			std::advance(iterator, Random::Bits(step, 2) % live.size());
			const uint32_t index = *iterator;

			retired.emplace_back(index, generations[index]);
			set.Free(index);
			live.erase(iterator);
		} else {
			const uint32_t index = set.Allocate();
			if (!live.insert(index).second) {
				duplicates++;
			}
			generations[index] = set.Generation(index);
		}

		if (set.LiveCount() != live.size()) {
			duplicates++;
		}
	}

	// Every handle that was retired must still read as dead, even where its
	// index has since been reissued several times.
	for (const auto &[index, generation] : retired) {
		if (set.Alive(index, generation)) {
			staleAlive++;
		}
	}

	REQUIRE(duplicates == 0);
	REQUIRE(staleAlive == 0);

	// And every index the model thinks is live is live at its recorded
	// generation.
	size_t missing = 0;
	for (const uint32_t index : live) {
		if (!set.Alive(index, generations[index])) {
			missing++;
		}
	}
	REQUIRE(missing == 0);
}
