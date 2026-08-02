#include <engine/core/Random.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.ecs.sparseset")

using engine::core::Random;
using engine::ecs::EntityLocation;
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
	// rather than only within the first block.
	const uint32_t count = SparseSet::PAGE_SIZE * 2 + 5;
	for (uint32_t index = 0; index < count; index++) {
		REQUIRE(set.Allocate() == index);
	}

	REQUIRE(set.LiveCount() == count);

	set.Relocate(SparseSet::PAGE_SIZE - 1, EntityLocation{1, 1});
	set.Relocate(SparseSet::PAGE_SIZE, EntityLocation{2, 2});
	set.Relocate(SparseSet::PAGE_SIZE * 2, EntityLocation{3, 3});

	REQUIRE(set.Locate(SparseSet::PAGE_SIZE - 1)->Archetype == 1);
	REQUIRE(set.Locate(SparseSet::PAGE_SIZE)->Archetype == 2);
	REQUIRE(set.Locate(SparseSet::PAGE_SIZE * 2)->Archetype == 3);
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
