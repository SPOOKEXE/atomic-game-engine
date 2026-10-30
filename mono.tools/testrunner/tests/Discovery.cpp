#include "SuiteDiscovery.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>
#include <catch2/internal/catch_test_registry.hpp>

#include <array>
#include <fstream>
#include <set>
#include <string_view>
#include <testrunner/Runner.hpp>

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

TEST_CASE("slow discovery keeps hidden GPU and same-stem cases eligible", "[discovery]") {
	Catch::TestCaseInfo ordinary("", {"ordinary", "[fast]"}, {"/one/Normal.cpp", 1});
	Catch::TestCaseInfo slow("", {"slow", "[SLOW]"}, {"/one/Slow.cpp", 1});
	Catch::TestCaseInfo hiddenGpu("", {"hidden gpu", "[.gpu][slow]"}, {"/two/Gpu.cpp", 1});
	Catch::TestCaseInfo duplicateStem("", {"other slow", "[slow]"}, {"/two/Shared.cpp", 1});
	const std::array cases{
		Catch::TestCaseHandle(&ordinary, nullptr),
		Catch::TestCaseHandle(&slow, nullptr),
		Catch::TestCaseHandle(&hiddenGpu, nullptr),
		Catch::TestCaseHandle(&duplicateStem, nullptr),
	};
	CHECK_FALSE(engine::testing::detail::HasSlowCases(cases, "/one/Normal.cpp"));
	CHECK(engine::testing::detail::HasSlowCases(cases, "/one/Slow.cpp"));
	CHECK(engine::testing::detail::HasSlowCases(cases, "/two/Gpu.cpp"));
	CHECK(engine::testing::detail::HasSlowCases(cases, "/one/shared.cpp"));
	CHECK(engine::testing::detail::HasSlowCases(cases, ""));
	CHECK(engine::testing::detail::HasSlowCases(cases, "relative/Normal.cpp"));

	Catch::TestCaseInfo unknown("", {"unusual path", "[slow]"}, {"WithoutExtension", 1});
	const std::array unknownCases{Catch::TestCaseHandle(&unknown, nullptr)};
	CHECK(engine::testing::detail::HasSlowCases(unknownCases, "/one/Normal.cpp"));
}

TEST_CASE("slow discovery uses the runner's canonical filename tag", "[discovery]") {
	testrunner::ReportScratchDirectory scratch(std::filesystem::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const auto target = scratch.Path() / "Real.cpp";
	const auto alias = scratch.Path() / "Alias.cpp";
	std::ofstream(target).put('\n');
	std::error_code error;
	std::filesystem::create_symlink(target, alias, error);
	if (error) {
		SUCCEED("symlinks unavailable on this platform");
		return;
	}
	const auto source = target.string();
	Catch::TestCaseInfo slow("", {"slow", "[slow]"}, {source.c_str(), 1});
	const std::array cases{Catch::TestCaseHandle(&slow, nullptr)};
	CHECK(engine::testing::detail::HasSlowCases(cases, alias.string()));
}

TEST_CASE("suite discovery covers the actual unfiltered Catch registry", "[discovery]") {
	CHECK(
		engine::testing::detail::MissingSuiteSources(
			Catch::getRegistryHub().getTestCaseRegistry().getAllTests(), engine::testing::Registry::All()
		)
			.empty()
	);
}
