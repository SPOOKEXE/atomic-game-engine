#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <testrunner/Process.hpp>
#include <vector>

TEST_SUITE_ID("tools.benchrunner.selection")

namespace {

	testrunner::ProcessResult RunFixture(std::vector<std::string> arguments = {}) {
		arguments.insert(arguments.begin(), BENCH_SELECTION_FIXTURE_PATH);
		arguments.insert(arguments.begin() + 1, {"--samples", "1", "--warmup-samples", "0"});
		return testrunner::Run(arguments);
	}

	void RequireNoBenchmarkRan(const testrunner::ProcessResult &result) {
		CHECK(result.Started);
		CHECK(result.ExitCode == 2);
		CHECK(result.Output.find("BODY_") == std::string::npos);
		CHECK(result.Output.find("# mono bench report") == std::string::npos);
	}
}

TEST_CASE("benchmark selection keeps default and suite-only runs", "[benchrunner]") {
	const auto all = RunFixture();
	REQUIRE(all.Started);
	REQUIRE(all.ExitCode == 0);
	CHECK(all.Output.find("BODY_ALPHA_SHARED") != std::string::npos);
	CHECK(all.Output.find("BODY_BETA_SHARED") != std::string::npos);
	CHECK(all.Output.find("BODY_ALPHA_EXTENDED") != std::string::npos);

	const auto suite = RunFixture({"--suite", "fixture.beta"});
	REQUIRE(suite.Started);
	REQUIRE(suite.ExitCode == 0);
	CHECK(suite.Output.find("BODY_BETA_SHARED") != std::string::npos);
	CHECK(suite.Output.find("BODY_ALPHA_SHARED") == std::string::npos);
	CHECK(suite.Output.find("BODY_ALPHA_EXTENDED") == std::string::npos);
}

TEST_CASE("benchmark name selection is exact and may match across suites", "[benchrunner]") {
	const auto shared = RunFixture({"--benchmark", "shared"});
	REQUIRE(shared.Started);
	REQUIRE(shared.ExitCode == 0);
	CHECK(shared.Output.find("BODY_ALPHA_SHARED") != std::string::npos);
	CHECK(shared.Output.find("BODY_BETA_SHARED") != std::string::npos);
	CHECK(shared.Output.find("BODY_ALPHA_EXTENDED") == std::string::npos);

	const auto intersection = RunFixture({"--suite", "fixture.beta", "--benchmark", "shared"});
	REQUIRE(intersection.Started);
	REQUIRE(intersection.ExitCode == 0);
	CHECK(intersection.Output.find("BODY_BETA_SHARED") != std::string::npos);
	CHECK(intersection.Output.find("BODY_ALPHA_SHARED") == std::string::npos);

	const auto extended = RunFixture({"--benchmark", "shared extended"});
	REQUIRE(extended.Started);
	REQUIRE(extended.ExitCode == 0);
	CHECK(extended.Output.find("BODY_ALPHA_EXTENDED") != std::string::npos);
	CHECK(extended.Output.find("BODY_ALPHA_SHARED") == std::string::npos);
}

TEST_CASE("invalid and unmatched benchmark selections fail before running", "[benchrunner]") {
	RequireNoBenchmarkRan(RunFixture({"--benchmark"}));
	RequireNoBenchmarkRan(RunFixture({"--benchmark", ""}));
	RequireNoBenchmarkRan(RunFixture({"--benchmark", "absent"}));
	RequireNoBenchmarkRan(RunFixture({"--benchmark", "share"}));
	RequireNoBenchmarkRan(RunFixture({"--suite", "fixture.beta", "--benchmark", "shared extended"}));
}
