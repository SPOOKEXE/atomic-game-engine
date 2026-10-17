#include <engine/render/InterfaceTargetCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.interfacetargetcache")

namespace {
	using namespace engine;
	using namespace engine::render;

	InterfaceTargetKey
	Key(uint64_t collector,
		uint64_t viewer = 1,
		uint32_t width = 100,
		uint32_t height = 50,
		size_t firstCommand = 0,
		size_t commandCount = 1) {
		return {
			.Collector = ecs::Entity(collector),
			.Viewer = viewer,
			.Width = width,
			.Height = height,
			.FirstCommand = firstCommand,
			.CommandCount = commandCount,
		};
	}

	gui::Compiled::DamageRegion Damage(uint64_t collector, float left, float top, float right, float bottom) {
		return {.Collector = ecs::Entity(collector), .Bounds = {left, top, right, bottom}};
	}
}

TEST_CASE("interface targets skip a completed unchanged collector", "[render][interface-target-cache]") {
	InterfaceTargetCache cache;
	int target = 0;
	REQUIRE(cache.Attach(Key(1), &target, 128, {}));
	CHECK(cache.Begin(Key(1), 7, true, {}).Work == InterfaceTargetWork::Full);
	cache.Complete(Key(1), true);
	CHECK(cache.Begin(Key(1), 7, true, {}).Work == InterfaceTargetWork::Skip);
}

TEST_CASE("interface target failure preserves conservative work", "[render][interface-target-cache]") {
	InterfaceTargetCache cache;
	int target = 0;
	REQUIRE(cache.Attach(Key(1), &target, 128, {}));
	cache.Begin(Key(1), 1, true, {});
	cache.Complete(Key(1), true);

	const auto damage = Damage(1, 4, 5, 20, 30);
	const auto partial = cache.Begin(Key(1), 2, true, {&damage, 1});
	REQUIRE(partial.Work == InterfaceTargetWork::Partial);
	REQUIRE(partial.Damage.size() == 1);
	cache.Complete(Key(1), false);
	CHECK(cache.Begin(Key(1), 2, true, {}).Work == InterfaceTargetWork::Partial);

	const auto full = cache.Begin(Key(1), 3, false, {});
	CHECK(full.Work == InterfaceTargetWork::Full);
	cache.Complete(Key(1), false);
	CHECK(cache.Begin(Key(1), 3, true, {}).Work == InterfaceTargetWork::Full);
}

TEST_CASE(
	"interface targets keep the previous image until an accepted completion",
	"[render][interface-target-cache]"
) {
	InterfaceTargetCache cache;
	int target = 0;
	const auto key = Key(7);
	REQUIRE(cache.Attach(key, &target, 128, {}));
	cache.Begin(key, 1, true, {});
	cache.Complete(key, true);
	REQUIRE(cache.Target(key) == &target);

	const auto damage = Damage(7, 1, 2, 8, 9);
	REQUIRE(cache.Begin(key, 2, true, {&damage, 1}).Work == InterfaceTargetWork::Partial);
	cache.Complete(key, false);
	CHECK(cache.Target(key) == &target);
	CHECK(cache.Begin(key, 2, true, {}).Work == InterfaceTargetWork::Partial);
	cache.Complete(key, true);
	CHECK(cache.Begin(key, 2, true, {}).Work == InterfaceTargetWork::Skip);
}

TEST_CASE("unchanged collector adopts a newer shared compile signature", "[render][interface-target-cache]") {
	InterfaceTargetCache cache;
	int changedTarget = 0;
	int unchangedTarget = 0;
	const auto changed = Key(11);
	const auto unchanged = Key(12);
	REQUIRE(cache.Attach(changed, &changedTarget, 128, {}));
	REQUIRE(cache.Attach(unchanged, &unchangedTarget, 128, {}));
	cache.Begin(changed, 1, true, {});
	cache.Complete(changed, true);
	cache.Begin(unchanged, 1, true, {});
	cache.Complete(unchanged, true);

	const auto damage = Damage(11, 4, 5, 20, 30);
	CHECK(cache.Begin(changed, 2, true, {&damage, 1}).Work == InterfaceTargetWork::Partial);
	CHECK(cache.Begin(unchanged, 2, true, {&damage, 1}).Work == InterfaceTargetWork::Skip);
	CHECK(cache.Begin(unchanged, 2, true, {}).Work == InterfaceTargetWork::Skip);
}

TEST_CASE(
	"interface targets are bounded and composite collector ranges in paint order",
	"[render][interface-target-cache]"
) {
	InterfaceTargetCache cache({.Count = 3, .Bytes = 384});
	int first = 0;
	int second = 0;
	int third = 0;
	REQUIRE(cache.Attach(Key(2, 1, 100, 50, 0, 2), &first, 128, {}));
	REQUIRE(cache.Attach(Key(3, 1, 100, 50, 2, 1), &second, 128, {}));
	cache.Begin(Key(2, 1, 100, 50, 0, 2), 1, true, {});
	cache.Complete(Key(2, 1, 100, 50, 0, 2), true);
	cache.Begin(Key(3, 1, 100, 50, 2, 1), 1, true, {});
	cache.Complete(Key(3, 1, 100, 50, 2, 1), true);
	REQUIRE(cache.Attach(Key(2, 1, 100, 50, 3, 1), &third, 128, {}));
	CHECK(cache.TargetCount() == 3);
	CHECK(cache.TargetBytes() == 384);

	gui::DrawList list;
	list.CollectorRanges = {
		{.Collector = ecs::Entity(2), .First = 0, .Count = 2},
		{.Collector = ecs::Entity(3), .First = 2, .Count = 1},
		{.Collector = ecs::Entity(2), .First = 3, .Count = 1},
	};
	cache.Begin(Key(2, 1, 100, 50, 3, 1), 1, true, {});
	cache.Complete(Key(2, 1, 100, 50, 3, 1), true);
	const auto composition = cache.Composite(list, 1, 100, 50);
	REQUIRE(composition.size() == 3);
	CHECK(composition[0].Target == &first);
	CHECK(composition[1].Target == &second);
	CHECK(composition[2].Target == &third);
}

TEST_CASE("planned target records stay within the target count bound", "[render][interface-target-cache]") {
	InterfaceTargetCache cache({.Count = 2, .Bytes = 256});
	cache.Begin(Key(1), 1, true, {});
	cache.Begin(Key(2), 1, true, {});
	cache.Begin(Key(3), 1, true, {});
	CHECK(cache.TargetCount() == 0);

	int target = 0;
	REQUIRE(cache.Attach(Key(3), &target, 128, {}));
	CHECK(cache.TargetCount() == 1);
}
