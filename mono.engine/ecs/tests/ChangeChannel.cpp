#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_SUITE_ID("engine.ecs.changechannel")

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
