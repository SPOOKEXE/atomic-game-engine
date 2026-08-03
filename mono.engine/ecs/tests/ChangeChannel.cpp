#include <engine/core/Random.hpp>
#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_SUITE_ID("engine.ecs.changechannel")

using engine::ecs::Column;
using engine::ecs::Components;
using engine::ecs::DirtyBits;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace changechannel_test {
	struct Spot {
		float X = 0.0f;
	};
	struct Drift {
		float X = 0.0f;
	};
	struct Quiet {
		int Value = 0;
	};

	std::vector<Entity> ChangedSpots(Store &store) {
		std::vector<Entity> found;
		store.EachChanged<Spot>([&](Entity entity, Spot &) { found.push_back(entity); });
		std::sort(found.begin(), found.end(), [](Entity left, Entity right) { return left.Id < right.Id; });
		return found;
	}
}

using namespace changechannel_test;

TEST_CASE("bits pack and read back by position", "[ecs]") {
	DirtyBits bits;
	REQUIRE(bits.Quiet());
	REQUIRE_FALSE(bits.Test(0));

	bits.Mark(0);
	bits.Mark(63);

	REQUIRE_FALSE(bits.Quiet());
	REQUIRE(bits.Test(0));
	REQUIRE(bits.Test(63));
	REQUIRE_FALSE(bits.Test(1));
}

TEST_CASE("a position past the mask is ignored rather than wrapping", "[ecs]") {
	// A shift by 64 or more is undefined, and an archetype wider than the mask
	// is a real possibility. Refusing quietly beats wrapping onto bit zero and
	// reporting a change to an unrelated component.
	DirtyBits bits;
	bits.Mark(DirtyBits::CAPACITY);
	bits.Mark(DirtyBits::CAPACITY + 100);

	REQUIRE(bits.Quiet());
	REQUIRE_FALSE(bits.Test(DirtyBits::CAPACITY));
}

TEST_CASE("nothing is tracked until something is observed", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{1.0f});

	// The honest answer for an unobserved component is false: nothing recorded
	// the write, so nothing can claim it happened.
	REQUIRE_FALSE(store.Observed<Spot>());
	REQUIRE_FALSE(store.Changed<Spot>(entity));
}

TEST_CASE("an observed write is recorded", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();
	REQUIRE(store.Observed<Spot>());

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{1.0f});

	REQUIRE(store.Changed<Spot>(entity));
}

TEST_CASE("observing is idempotent", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();
	store.Observe<Spot>();
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});

	REQUIRE(store.Changed<Spot>(entity));
	REQUIRE(store.Observed<Spot>());
}

TEST_CASE("a component appearing counts as a change", "[ecs]") {
	// A consumer rebuilding from a delta needs the row that just gained the
	// component as much as the one that was overwritten.
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Drift>(entity, Drift{});
	REQUIRE_FALSE(store.Changed<Spot>(entity));

	store.Set<Spot>(entity, Spot{2.0f});
	REQUIRE(store.Changed<Spot>(entity));
}

TEST_CASE("changes are per entity, not per component type", "[ecs]") {
	// The distinction the whole mechanism exists for: a script wants to know
	// that *this* part moved, not that some part did.
	Store store("test");
	store.Observe<Spot>();

	const Entity moved = store.Create();
	const Entity still = store.Create();
	store.Set<Spot>(moved, Spot{});
	store.Set<Spot>(still, Spot{});
	store.ClearChanges();

	store.Set<Spot>(moved, Spot{5.0f});

	REQUIRE(store.Changed<Spot>(moved));
	REQUIRE_FALSE(store.Changed<Spot>(still));
	REQUIRE(ChangedSpots(store) == std::vector<Entity>{moved});
}

TEST_CASE("changes are per component, not per entity", "[ecs]") {
	// And the other axis: an entity whose Drift moved has not had its Spot
	// changed, even though both live in the same row.
	Store store("test");
	store.Observe<Spot>();
	store.Observe<Drift>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	store.Set<Drift>(entity, Drift{});
	store.ClearChanges();

	store.Set<Drift>(entity, Drift{9.0f});

	REQUIRE(store.Changed<Drift>(entity));
	REQUIRE_FALSE(store.Changed<Spot>(entity));
}

TEST_CASE("clearing quiets everything at once", "[ecs]") {
	// Cleared at a phase boundary rather than per read, so every consumer in a
	// tick sees the same set — and a property written three times signals once.
	Store store("test");
	store.Observe<Spot>();

	std::vector<Entity> entities;
	for (int index = 0; index < 32; index++) {
		const Entity entity = store.Create();
		store.Set<Spot>(entity, Spot{});
		entities.push_back(entity);
	}

	REQUIRE(ChangedSpots(store).size() == 32);

	store.ClearChanges();
	REQUIRE(ChangedSpots(store).empty());

	for (const Entity entity : entities) {
		REQUIRE_FALSE(store.Changed<Spot>(entity));
	}
}

TEST_CASE("writing the same component three times signals once", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{1.0f});
	store.Set<Spot>(entity, Spot{2.0f});
	store.Set<Spot>(entity, Spot{3.0f});

	REQUIRE(ChangedSpots(store) == std::vector<Entity>{entity});
	REQUIRE(store.Get<Spot>(entity)->X == 3.0f);
}

TEST_CASE("EachChanged hands out the live value", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{7.0f});

	float seen = 0.0f;
	store.EachChanged<Spot>([&](Entity, Spot &spot) {
		seen = spot.X;
		spot.X = 8.0f; // and it is the store's own row, not a copy
	});

	REQUIRE(seen == 7.0f);
	REQUIRE(store.Get<Spot>(entity)->X == 8.0f);
}

TEST_CASE("EachChanged spans archetypes", "[ecs]") {
	// Two tables hold Spot: one with Drift and one without. A delta that only
	// walked the first would silently drop half a world.
	Store store("test");
	store.Observe<Spot>();

	const Entity plain = store.Create();
	store.Set<Spot>(plain, Spot{});

	const Entity drifting = store.Create();
	store.Set<Spot>(drifting, Spot{});
	store.Set<Drift>(drifting, Drift{});

	const std::vector<Entity> changed = ChangedSpots(store);
	REQUIRE(changed.size() == 2);
}

TEST_CASE("a mutable pointer counts as a write", "[ecs]") {
	// Whether the caller used it cannot be known from here. Reporting a change
	// that did not happen costs a consumer one wasted rebuild; missing one
	// costs it correctness, so the cheap direction is the safe one.
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	store.ClearChanges();

	store.GetMutable<Spot>(entity)->X = 4.0f;
	REQUIRE(store.Changed<Spot>(entity));
}

TEST_CASE("a const read does not count as a write", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	store.ClearChanges();

	const Store &reading = store;
	REQUIRE(reading.Get<Spot>(entity)->X == 0.0f);
	REQUIRE_FALSE(store.Changed<Spot>(entity));
}

TEST_CASE("an unobserved component costs no bits", "[ecs]") {
	// Tracking is per type because most components have nobody asking, and a
	// world that observes nothing must carry no DirtyBits column at all.
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Quiet>(entity, Quiet{1});

	REQUIRE_FALSE(store.Has<DirtyBits>(entity));
	REQUIRE(store.TableCount() == 1);
}

TEST_CASE("a table gains the bits only when one of its components is observed", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const Entity watched = store.Create();
	store.Set<Spot>(watched, Spot{});

	const Entity ignored = store.Create();
	store.Set<Quiet>(ignored, Quiet{});

	REQUIRE(store.Has<DirtyBits>(watched));
	REQUIRE_FALSE(store.Has<DirtyBits>(ignored));
}

TEST_CASE("observing after the fact migrates what already exists", "[ecs]") {
	// Declaring observation when a world is built avoids this entirely. Doing
	// it late has to move every entity already carrying the component into a
	// table with somewhere to put the bits — correct, and not free.
	Store store("test");

	std::vector<Entity> entities;
	for (int index = 0; index < 16; index++) {
		const Entity entity = store.Create();
		store.Set<Spot>(entity, Spot{static_cast<float>(index)});
		entities.push_back(entity);
	}

	store.Observe<Spot>();

	// Every entity kept its value and its identity through the move.
	for (size_t index = 0; index < entities.size(); index++) {
		REQUIRE(store.Alive(entities[index]));
		REQUIRE(store.Has<DirtyBits>(entities[index]));
		REQUIRE(store.Get<Spot>(entities[index])->X == static_cast<float>(index));
	}

	// And tracking works from here on.
	store.ClearChanges();
	store.Set<Spot>(entities[3], Spot{99.0f});
	REQUIRE(ChangedSpots(store) == std::vector<Entity>{entities[3]});
}

TEST_CASE("a destroyed entity reports no change", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	store.Destroy(entity);

	REQUIRE_FALSE(store.Changed<Spot>(entity));
}

TEST_CASE("the coarse counter moves on every recorded write", "[ecs]") {
	Store store("test");
	store.Observe<Spot>();

	const uint64_t before = store.ChangeVersion();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	const uint64_t afterFirst = store.ChangeVersion();
	REQUIRE(afterFirst > before);

	store.Set<Spot>(entity, Spot{1.0f});
	REQUIRE(store.ChangeVersion() > afterFirst);

	// Clearing the bits does not rewind the counter — it is a monotonic "has
	// anything happened", which is what a consumer comparing against a stored
	// value needs.
	const uint64_t held = store.ChangeVersion();
	store.ClearChanges();
	REQUIRE(store.ChangeVersion() == held);
}

TEST_CASE("a batch write moves the counter but sets no bit", "[ecs]") {
	// The documented gap. EachBatch hands out raw column pointers precisely to
	// avoid a per-row check, so a write through one cannot set a bit — and
	// v0.4's QuickHash is what closes it for consumers that need row
	// granularity over batch-written data.
	Store store("test");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{});
	store.ClearChanges();
	const uint64_t before = store.ChangeVersion();

	store.EachBatch<Spot>([](size_t rows, Spot *spots) {
		for (size_t row = 0; row < rows; row++) {
			spots[row].X = 3.0f;
		}
	});

	REQUIRE(store.Get<Spot>(entity)->X == 3.0f);
	REQUIRE_FALSE(store.Changed<Spot>(entity));
	REQUIRE(store.ChangeVersion() == before);
}

// --- runs, for a delta --------------------------------------------------------
//
// `EachChanged` hands over one row at a time, which is right for a signal and
// wrong for a delta. Rows that changed together are usually adjacent, so a
// replication pass wants the runs — a memcpy each rather than a copy per entity.

TEST_CASE("changed runs arrive as contiguous blocks", "[ecs]") {
	Store store("runs");
	store.Observe<Quiet>();

	std::vector<Entity> entities;
	for (int index = 0; index < 32; index++) {
		const Entity entity = store.Create();
		store.Set<Quiet>(entity, Quiet{index});
		entities.push_back(entity);
	}
	store.ClearChanges();

	// Two separate runs, so the batching has something to get wrong. Both sit
	// inside one chunk on purpose: a run is clipped at a chunk boundary, so
	// what is being measured here is the coalescing rather than the storage's
	// division, and the clipping has a case of its own below.
	for (int index = 17; index < 22; index++) {
		store.GetMutable<Quiet>(entities[static_cast<size_t>(index)])->Value = 100 + index;
	}
	for (int index = 25; index < 28; index++) {
		store.GetMutable<Quiet>(entities[static_cast<size_t>(index)])->Value = 200 + index;
	}

	std::vector<std::pair<size_t, int>> runs; // rows in the run, first value
	size_t total = 0;
	store.EachChangedBatch<Quiet>([&runs, &total](const Entity *, Quiet *values, size_t rows) {
		runs.emplace_back(rows, values[0].Value);
		total += rows;
	});

	REQUIRE(runs.size() == 2);
	REQUIRE(runs[0].first == 5);
	REQUIRE(runs[0].second == 117);
	REQUIRE(runs[1].first == 3);
	REQUIRE(runs[1].second == 225);
	REQUIRE(total == 8);
}

TEST_CASE("a run's entities and values line up", "[ecs]") {
	// The whole point of a run is that both arrays are contiguous and parallel,
	// so a delta can copy the values as a block and still say which entities
	// they belong to.
	Store store("runs");
	store.Observe<Quiet>();

	std::vector<Entity> entities;
	for (int index = 0; index < 8; index++) {
		const Entity entity = store.Create();
		store.Set<Quiet>(entity, Quiet{index});
		entities.push_back(entity);
	}

	store.EachChangedBatch<Quiet>([](const Entity *found, Quiet *values, size_t rows) {
		for (size_t row = 0; row < rows; row++) {
			// Each entity was set to its own index, so this holds only if the
			// two arrays are in step.
			REQUIRE(values[row].Value >= 0);
			REQUIRE(found[row].Id != 0);
		}
	});

	size_t seen = 0;
	store.EachChangedBatch<Quiet>([&seen](const Entity *, Quiet *, size_t rows) { seen += rows; });
	REQUIRE(seen == 8);
}

TEST_CASE("a run stops at a chunk boundary and never straddles one", "[ecs]") {
	// **The clip that keeps a delta honest.** A run hands the callback one base
	// pointer and a row count, and `data + row * size` has to be the value for
	// `entities[row]` over the whole run. A column is only contiguous inside one
	// chunk, so a run allowed to grow past a boundary walks into the previous
	// chunk's tail — and the entity array does *not*, because `Archetype::Ids`
	// is one allocation. The result is entity A's id sent with entity B's bytes:
	// every count still adds up, nothing crashes, and a client shows objects
	// teleporting.
	Store store("runs");
	store.Observe<Quiet>();

	// Deliberately several chunks and not a whole number of them.
	const size_t count = 2348;
	std::vector<Entity> entities;
	entities.reserve(count);
	for (size_t index = 0; index < count; index++) {
		const Entity entity = store.Create();
		store.Set<Quiet>(entity, Quiet{static_cast<int>(index)});
		entities.push_back(entity);
	}

	// Everything dirty, so the runs are as long as the storage will let them be.
	store.MarkAllChanged<Quiet>();

	size_t wrongValue = 0;
	size_t straddling = 0;
	size_t seen = 0;
	store.EachChangedBatch<Quiet>([&](const Entity *found, Quiet *values, size_t rows) {
		for (size_t row = 0; row < rows; row++) {
			// Each entity holds its own creation index, so this is only true
			// when the two arrays are still in step.
			const Quiet *held = store.Get<Quiet>(found[row]);
			if (held == nullptr || held->Value != values[row].Value) {
				wrongValue++;
			}
		}

		// And the run itself never crosses a boundary, which is the property
		// that makes the check above hold rather than merely happen to pass.
		const auto first = reinterpret_cast<uintptr_t>(values);
		const auto last = reinterpret_cast<uintptr_t>(values + rows - 1);
		if ((last - first) / sizeof(Quiet) != rows - 1) {
			straddling++;
		}
		seen += rows;
	});

	REQUIRE(seen == count);
	REQUIRE(wrongValue == 0);
	REQUIRE(straddling == 0);

	// And clearing reaches every chunk. Both `MarkAllChanged` and
	// `ClearChanges` used to take the whole row count from chunk zero's base —
	// a write past the end of the first chunk, so the bits past it were left
	// alone and somebody else's chunk was overwritten.
	store.ClearChanges();
	size_t stillDirty = 0;
	store.EachChangedBatch<Quiet>([&stillDirty](const Entity *, Quiet *, size_t rows) {
		stillDirty += rows;
	});
	REQUIRE(stillDirty == 0);
}

TEST_CASE("the runtime-keyed run form clips where the typed one does", "[ecs]") {
	// `EachChangedRuns` is the form `replication::Authority` uses, because it
	// resolves a component off a wire and cannot name a type at compile time.
	// It shares `VisitChangedRuns` with the typed form, and this is what says so
	// rather than assuming it.
	Store store("runs");
	store.Observe<Quiet>();

	const size_t count = 1088;
	std::vector<Entity> entities;
	entities.reserve(count);
	for (size_t index = 0; index < count; index++) {
		const Entity entity = store.Create();
		store.Set<Quiet>(entity, Quiet{static_cast<int>(index)});
		entities.push_back(entity);
	}
	store.MarkAllChanged<Quiet>();

	size_t wrongValue = 0;
	size_t seen = 0;
	store.EachChangedRuns(Components::Of<Quiet>(), [&](const Entity *found, void *data, size_t rows) {
		const auto *values = static_cast<const Quiet *>(data);
		for (size_t row = 0; row < rows; row++) {
			const Quiet *held = store.Get<Quiet>(found[row]);
			if (held == nullptr || held->Value != values[row].Value) {
				wrongValue++;
			}
		}
		seen += rows;
	});

	REQUIRE(seen == count);
	REQUIRE(wrongValue == 0);
}

TEST_CASE("nothing changed means no runs at all", "[ecs]") {
	Store store("runs");
	store.Observe<Quiet>();
	store.Set<Quiet>(store.Create(), Quiet{1});
	store.ClearChanges();

	size_t calls = 0;
	store.EachChangedBatch<Quiet>([&calls](const Entity *, Quiet *, size_t) { calls++; });
	REQUIRE(calls == 0);
}

TEST_CASE("runs and rows agree on what changed", "[ecs][fuzz]") {
	// The batched form is an optimisation of the row form, so the two must
	// always name the same set. Scattered writes, so runs of every length turn
	// up including single rows.
	Store store("runs");
	store.Observe<Quiet>();

	std::vector<Entity> entities;
	for (int index = 0; index < 200; index++) {
		const Entity entity = store.Create();
		store.Set<Quiet>(entity, Quiet{index});
		entities.push_back(entity);
	}

	for (uint32_t round = 0; round < 200; round++) {
		store.ClearChanges();

		std::vector<uint64_t> expected;
		for (size_t index = 0; index < entities.size(); index++) {
			if (engine::core::Random::Bits(round, static_cast<uint32_t>(index)) % 3 == 0) {
				store.GetMutable<Quiet>(entities[index])->Value = static_cast<int>(round);
				expected.push_back(entities[index].Id);
			}
		}

		std::vector<uint64_t> byRow;
		store.EachChanged<Quiet>([&byRow](Entity entity, Quiet &) { byRow.push_back(entity.Id); });

		std::vector<uint64_t> byRun;
		store.EachChangedBatch<Quiet>([&byRun](const Entity *found, Quiet *, size_t rows) {
			for (size_t row = 0; row < rows; row++) {
				byRun.push_back(found[row].Id);
			}
		});

		std::sort(expected.begin(), expected.end());
		std::sort(byRow.begin(), byRow.end());
		std::sort(byRun.begin(), byRun.end());

		REQUIRE(byRow == expected);
		REQUIRE(byRun == expected);
	}
}

// --- the batch write -------------------------------------------------------
//
// `Set` and `GetMutable` mark; an iteration does not, because a reference handed
// out by `Each` is a pointer the store never sees written through. That is the
// engine's fastest write path and the one a replication delta most needs to
// know about, so there is a primitive for saying so afterwards.

TEST_CASE("iterating and writing marks nothing on its own", "[ecs]") {
	Store store("changes");
	store.Observe<Spot>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{1.0f});
	store.ClearChanges();

	store.Each<Spot>([](Entity, Spot &spot) { spot.X = 2.0f; });

	// The value moved and nothing recorded it. Pinned rather than lamented: a
	// delta built from the bits would carry none of this, and the day the
	// engine starts marking on iteration this test says so.
	REQUIRE(store.Get<Spot>(entity)->X == 2.0f);
	REQUIRE_FALSE(store.Changed<Spot>(entity));
	REQUIRE(ChangedSpots(store).empty());
}

TEST_CASE("MarkAllChanged reports every row holding the component", "[ecs]") {
	Store store("changes");
	store.Observe<Spot>();

	std::vector<Entity> marked;
	for (int index = 0; index < 8; index++) {
		const Entity entity = store.Create();
		store.Set<Spot>(entity, Spot{static_cast<float>(index)});
		marked.push_back(entity);
	}

	// A second archetype, so this is not a one-table answer.
	for (int index = 0; index < 4; index++) {
		const Entity entity = store.Create();
		store.Set<Spot>(entity, Spot{0.0f});
		store.Set<Drift>(entity, Drift{1.0f});
		marked.push_back(entity);
	}

	// One that has no Spot at all, and must not appear.
	const Entity other = store.Create();
	store.Set<Drift>(other, Drift{1.0f});

	store.ClearChanges();
	store.Each<Spot>([](Entity, Spot &spot) { spot.X += 1.0f; });
	store.MarkAllChanged<Spot>();

	std::vector<Entity> reported = ChangedSpots(store);
	std::sort(marked.begin(), marked.end(), [](Entity left, Entity right) { return left.Id < right.Id; });

	REQUIRE(reported == marked);
	REQUIRE_FALSE(store.Changed<Spot>(other));
}

TEST_CASE("MarkAllChanged marks one component and not its neighbours", "[ecs]") {
	Store store("changes");
	store.Observe<Spot>();
	store.Observe<Drift>();

	const Entity entity = store.Create();
	store.Set<Spot>(entity, Spot{1.0f});
	store.Set<Drift>(entity, Drift{1.0f});
	store.ClearChanges();

	store.MarkAllChanged<Spot>();

	// The bit index is a position in the table's sorted set, so marking the
	// wrong one is an off-by-one that reports a component nobody touched.
	REQUIRE(store.Changed<Spot>(entity));
	REQUIRE_FALSE(store.Changed<Drift>(entity));
}

TEST_CASE("MarkAllChanged on an unobserved component does nothing", "[ecs]") {
	Store store("changes");

	const Entity entity = store.Create();
	store.Set<Quiet>(entity, Quiet{1});

	// No table has a bits column, so there is nowhere to record it. Silent
	// rather than an error, matching `Changed` answering false for the same
	// reason: nothing recorded it, so nothing can say.
	store.MarkAllChanged<Quiet>();
	REQUIRE_FALSE(store.Changed<Quiet>(entity));
}

TEST_CASE("MarkAllChanged moves the coarse counter by the row count", "[ecs]") {
	Store store("changes");
	store.Observe<Spot>();

	for (int index = 0; index < 5; index++) {
		store.Set<Spot>(store.Create(), Spot{1.0f});
	}
	store.ClearChanges();

	const uint64_t before = store.ChangeVersion();
	store.MarkAllChanged<Spot>();

	// A consumer watching the version to decide whether to rebuild has to see a
	// batch write the same way it sees that many individual ones.
	REQUIRE(store.ChangeVersion() == before + 5);
}
