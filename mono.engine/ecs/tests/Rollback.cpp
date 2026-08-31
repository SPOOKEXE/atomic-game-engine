#include <engine/ecs/Rollback.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.ecs.rollback")

using engine::ecs::Entity;
using engine::ecs::RollbackBuffer;
using engine::ecs::Store;

namespace rollback_test {
	struct Counter {
		int Value = 0;
	};
}

using rollback_test::Counter;

TEST_CASE("rollback restores complete world state", "[ecs]") {
	Store store("simulation");
	const Entity entity = store.Create();
	store.Set<Counter>(entity, Counter{1});

	RollbackBuffer history(4);
	REQUIRE(history.Capture(store, 10));
	store.Set<Counter>(entity, Counter{2});
	store.AdvanceTick(0.016f);
	REQUIRE(history.Capture(store, 11));
	store.Set<Counter>(entity, Counter{3});

	REQUIRE(history.Restore(store, 10));
	REQUIRE(store.Alive(entity));
	REQUIRE(store.Get<Counter>(entity)->Value == 1);
	REQUIRE(store.Time().Tick == 0);
	REQUIRE(store.Name() == "simulation");
}

TEST_CASE("capturing a past tick branches bounded history", "[ecs]") {
	Store store("simulation");
	const Entity entity = store.Create();
	store.Set<Counter>(entity, Counter{0});
	RollbackBuffer history(3);

	for (uint64_t tick = 1; tick <= 4; tick++) {
		store.Set<Counter>(entity, Counter{static_cast<int>(tick)});
		REQUIRE(history.Capture(store, tick));
	}

	REQUIRE(history.Size() == 3);
	REQUIRE(history.OldestTick() == 2);
	REQUIRE(history.LatestTick() == 4);
	REQUIRE_FALSE(history.Restore(store, 1));

	store.Set<Counter>(entity, Counter{30});
	REQUIRE(history.Capture(store, 3));
	REQUIRE(history.LatestTick() == 3);
	REQUIRE(history.Size() == 2);
	REQUIRE_FALSE(history.Restore(store, 4));
	REQUIRE(history.Restore(store, 3));
	REQUIRE(store.Get<Counter>(entity)->Value == 30);
}

TEST_CASE("zero-capacity rollback refuses capture", "[ecs]") {
	Store store("simulation");
	RollbackBuffer history(0);

	REQUIRE_FALSE(history.Capture(store, 1));
	REQUIRE(history.Size() == 0);
	REQUIRE_FALSE(history.OldestTick().has_value());
}
