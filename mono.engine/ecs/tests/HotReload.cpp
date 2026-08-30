#include <engine/ecs/HotReload.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.ecs.hotreload")

using namespace engine::ecs;

namespace hot_reload_test {
	struct Health {
		int Maximum = 0;
		int Current = 0;
	};
}

using hot_reload_test::Health;

TEST_CASE("component reload migrates every value and publishes changes", "[ecs][reload]") {
	Store store("simulation");
	store.Observe<Health>();
	const Entity first = store.Create();
	const Entity second = store.Create();
	store.Set<Health>(first, Health{100, 50});
	store.Set<Health>(second, Health{200, 100});
	store.ClearChanges();

	ComponentReloads reloads;
	REQUIRE(reloads.Apply<Health>(store, 2, [](Entity, Health &health) {
		health.Current = health.Current * 2;
	}));
	CHECK(reloads.Current<Health>() == 2);
	CHECK(store.Get<Health>(first)->Current == 100);
	CHECK(store.Get<Health>(second)->Current == 200);
	CHECK(store.Changed<Health>(first));
	CHECK(store.Changed<Health>(second));
}

TEST_CASE("component reload refuses stale revisions", "[ecs][reload]") {
	Store store("simulation");
	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{100, 50});
	ComponentReloads reloads;
	REQUIRE(reloads.Apply<Health>(store, 3, [](Entity, Health &health) { health.Current = 75; }));

	CHECK_FALSE(reloads.Apply<Health>(store, 3, [](Entity, Health &health) { health.Current = 0; }));
	CHECK_FALSE(reloads.Apply<Health>(store, 2, [](Entity, Health &health) { health.Current = 0; }));
	CHECK(store.Get<Health>(entity)->Current == 75);
}
