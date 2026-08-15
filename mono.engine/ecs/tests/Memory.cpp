// What a world holds when nobody is looking at it.
//
// **The figure that justifies chunked storage had no test behind it anywhere in
// the repository.** `ROADMAP.md` records a thousand worlds that each grew to ten
// thousand entities and settled back at a hundred holding **703 MB against
// 2.7 MB of live rows**, and nothing measured it again afterwards - so the day
// somebody put the high-water mark back, the number in the roadmap would still
// have said it was fixed.
//
// Measured in bytes the storage says it holds rather than in process resident
// size. Resident size is the honest end figure and it is the wrong thing for a
// test: it moves with the allocator, the page size and whatever else ran first,
// so a threshold on it either fails on somebody's machine or is loose enough to
// pass through a regression. `Store::ResidentStorageBytes` is the same
// accounting with the noise taken out, and it is the accessor `SparseSet::
// ResidentSlots` set the precedent for.
//
// Measured on **one** world, because the aggregate is multiplication. What makes
// a thousand worlds expensive is the per-world overhang, and pinning it per
// world is what keeps the case runnable in a debug build.

#include "ChunkPool.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.ecs.memory")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.ecs.column")

using engine::ecs::ChunkPool;
using engine::ecs::Column;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace memory_test {
	// Three components of the widths a real scene has: a twelve-byte vector, a
	// sixteen-byte one and a four-byte scalar. Their own types rather than
	// `scene`'s, because `ecs` is at L3 and cannot see L7 - and because a
	// storage measurement should not move when a component gains a field.
	struct Place {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

	struct Drift {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float W = 0.0f;
	};

	struct Kind {
		uint32_t Value = 0;
	};

	// Bytes one row of that archetype costs while it is live: the three columns
	// plus the entity id array beside them.
	constexpr size_t LIVE_ROW_BYTES = sizeof(Place) + sizeof(Drift) + sizeof(Kind) + sizeof(Entity);

	std::vector<Entity> Grow(Store &store, size_t entities) {
		std::vector<Entity> made;
		made.reserve(entities);
		for (size_t index = 0; index < entities; index++) {
			const Entity entity = store.Create();
			store.Set<Place>(entity, Place{static_cast<float>(index), 0.0f, 0.0f});
			store.Set<Drift>(entity, Drift{});
			store.Set<Kind>(entity, Kind{static_cast<uint32_t>(index)});
			made.push_back(entity);
		}
		return made;
	}

	void SettleTo(Store &store, std::vector<Entity> &made, size_t keep) {
		while (made.size() > keep) {
			store.Destroy(made.back());
			made.pop_back();
		}
	}
}

using namespace memory_test;

TEST_CASE("a world that peaked and settled gives the peak back", "[ecs]") {
	// **The shape the item is filed against**, at one world's scale. Before
	// chunking, a column that reached ten thousand rows held sixteen thousand
	// forever, so this world stayed at its peak for as long as it existed.
	Store store("settled");
	std::vector<Entity> made = Grow(store, 10'000);

	const size_t atPeak = store.ResidentStorageBytes();
	REQUIRE(atPeak > 10'000 * LIVE_ROW_BYTES);

	SettleTo(store, made, 100);
	REQUIRE(store.CountMatching<Place, Drift, Kind>() == 100);

	const size_t settled = store.ResidentStorageBytes();

	// A hundred rows fit in one chunk per column, so what is left is one chunk
	// each, a trimmed id array and the directory's small first page. The bound
	// is the arithmetic rather than the measurement: anything above it means a
	// column, the id array or the directory has gone back to holding its peak.
	//
	// Sixteen kilobytes of slack covers the 8 KB first directory page and the id
	// array, which the trim rule leaves at no more than four times the rows.
	const size_t settledRows =
		Column::ChunkStart(Column::ChunksFor(100) - 1) + Column::ChunkRows(Column::ChunksFor(100) - 1);
	REQUIRE(settled < settledRows * LIVE_ROW_BYTES + 16 * 1024);
	REQUIRE(settled * 50 < atPeak);
}

TEST_CASE("a world that was always small stays small", "[ecs]") {
	// The other row of the same table, and the one v0.2 already fixed by
	// shrinking the directory's first page. It is here so that a change to
	// either half is measured against both shapes rather than only the one it
	// was aimed at.
	Store store("small");
	std::vector<Entity> made = Grow(store, 100);

	// One chunk per column plus one small directory page. Ninety-six kilobytes
	// would mean a column had gone back to allocating for its high-water mark.
	REQUIRE(store.ResidentStorageBytes() < 64 * 1024);
	REQUIRE(made.size() == 100);
}

TEST_CASE("settling a world does not hand the bytes to the pool instead", "[ecs]") {
	// **A pool that never trims relocates the leak and reports success.** The
	// chunks a settling world gives up land in a process-wide freelist, and if
	// that freelist were unbounded the thousand-world host would hold exactly
	// what it held before under a different name.
	ChunkPool::Trim();

	Store store("settled");
	std::vector<Entity> made = Grow(store, 10'000);
	SettleTo(store, made, 100);

	REQUIRE(ChunkPool::RetainedBytes() <= ChunkPool::RETAINED_BYTES_CAP);

	// And the retained spares are shared, not per world: a second world of the
	// same shape reuses them rather than adding its own.
	const size_t afterOne = ChunkPool::RetainedBytes();
	{
		Store second("settled-too");
		std::vector<Entity> more = Grow(second, 10'000);
		SettleTo(second, more, 100);
	}

	// The second world's chunks came back too, and the total is still bounded
	// by the cap rather than by the number of worlds.
	REQUIRE(ChunkPool::RetainedBytes() <= ChunkPool::RETAINED_BYTES_CAP);
	REQUIRE(afterOne <= ChunkPool::RETAINED_BYTES_CAP);

	ChunkPool::Trim();
	REQUIRE(ChunkPool::RetainedBytes() == 0);
}

TEST_CASE("a chunk is the unit a column grows and shrinks by", "[ecs]") {
	// The mechanism behind the two cases above, stated once so that a failure
	// there has somewhere to point.
	Store store("stepped");
	std::vector<Entity> made = Grow(store, 4096);

	const size_t atFour = store.ResidentStorageBytes();
	SettleTo(store, made, 2048);
	const size_t atTwo = store.ResidentStorageBytes();
	SettleTo(store, made, 1024);
	const size_t atOne = store.ResidentStorageBytes();

	REQUIRE(atTwo < atFour);
	REQUIRE(atOne < atTwo);

	// Halving the population halves the storage, because the chunk that goes
	// back is the largest one and it holds half the rows. That is the property
	// doubling chunks have and a fixed chunk size does not.
}
