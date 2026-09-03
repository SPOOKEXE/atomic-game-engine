#include <engine/ecs/Store.hpp>
#include <engine/script/Clock.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.script.clock")

TEST_CASE("the script clock follows the world rate by default", "[script][clock]") {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store store("script_clock_test");
	store.AdvanceTick(1.0f / 60.0f);

	engine::script::SetScriptTickRate(store, 0.0);
	const auto delta = engine::script::TakeScriptUpdate(store);
	REQUIRE(delta);
	CHECK(*delta == Catch::Approx(1.0f / 60.0f));
}

TEST_CASE("the script clock accumulates and drains independent updates", "[script][clock]") {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store store("script_clock_test");
	engine::script::SetScriptTickRate(store, 120.0);

	store.AdvanceTick(1.0f / 60.0f);
	const auto first = engine::script::TakeScriptUpdate(store);
	const auto second = engine::script::TakeScriptUpdate(store);
	REQUIRE(first);
	REQUIRE(second);
	CHECK(*first == Catch::Approx(1.0f / 120.0f));
	CHECK(*second == Catch::Approx(1.0f / 120.0f));
	CHECK_FALSE(engine::script::TakeScriptUpdate(store));
}

TEST_CASE("the script clock waits until a lower-rate update is due", "[script][clock]") {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store store("script_clock_test");
	engine::script::SetScriptTickRate(store, 30.0);

	store.AdvanceTick(1.0f / 60.0f);
	CHECK_FALSE(engine::script::TakeScriptUpdate(store));

	store.AdvanceTick(1.0f / 60.0f);
	const auto delta = engine::script::TakeScriptUpdate(store);
	REQUIRE(delta);
	CHECK(*delta == Catch::Approx(1.0f / 30.0f));
}

TEST_CASE("the script clock rejects non-finite rates", "[script][clock]") {
	engine::script::RegisterScriptComponents();
	engine::ecs::Store store("script_clock_test");
	store.AdvanceTick(1.0f / 60.0f);

	engine::script::SetScriptTickRate(store, std::numeric_limits<double>::infinity());
	const auto delta = engine::script::TakeScriptUpdate(store);
	REQUIRE(delta);
	CHECK(*delta == Catch::Approx(1.0f / 60.0f));
}
