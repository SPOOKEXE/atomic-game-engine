#include "SuiteDiscovery.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>
#include <catch2/internal/catch_test_registry.hpp>

#include <array>
#include <set>
#include <string_view>

TEST_SUITE_ID("tools.testrunner.discovery")

TEST_CASE("suite discovery requires every ordinary and hidden source", "[discovery]") {
	Catch::TestCaseInfo ordinary("", {"ordinary", ""}, {"/first/Shared.cpp", 1});
	Catch::TestCaseInfo duplicate("", {"another ordinary", ""}, {"/first/Shared.cpp", 2});
	Catch::TestCaseInfo hidden("", {"hidden", "[.hidden]"}, {"/second/Shared.cpp", 1});
	REQUIRE(hidden.isHidden());
	const std::array cases{
		Catch::TestCaseHandle(&ordinary, nullptr),
		Catch::TestCaseHandle(&duplicate, nullptr),
		Catch::TestCaseHandle(&hidden, nullptr),
	};
	const auto missing = engine::testing::detail::MissingSuiteSources(cases, {});
	CHECK(missing == std::set<std::string_view>{"/first/Shared.cpp", "/second/Shared.cpp"});

	const std::array declared{engine::testing::Suite{"first", "/first/Shared.cpp", {}, {}, {}}};
	CHECK(
		engine::testing::detail::MissingSuiteSources(cases, declared) ==
		std::set<std::string_view>{"/second/Shared.cpp"}
	);

	const std::array complete{
		declared[0], engine::testing::Suite{"hidden", "/second/Shared.cpp", {}, {}, {}}
	};
	CHECK(engine::testing::detail::MissingSuiteSources(cases, complete).empty());
}

TEST_CASE("suite discovery permits declared sources without cases", "[discovery]") {
	const std::array declared{
		engine::testing::Suite{"empty", "/tests/Empty.cpp", {}, {}, {}},
		engine::testing::Suite{"no-source", "", {}, {}, {}},
	};
	CHECK(engine::testing::detail::MissingSuiteSources({}, declared).empty());
}

TEST_CASE("suite discovery covers the actual unfiltered Catch registry", "[discovery]") {
	CHECK(
		engine::testing::detail::MissingSuiteSources(
			Catch::getRegistryHub().getTestCaseRegistry().getAllTests(), engine::testing::Registry::All()
		)
			.empty()
	);
}
