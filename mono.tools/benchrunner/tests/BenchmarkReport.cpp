#include "BenchmarkReport.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("tools.benchrunner.report")

TEST_CASE("benchmark runner accepts complete selected-suite rows", "[benchrunner]") {
	std::vector<benchrunner::Measurement> measurements;
	std::string error;
	REQUIRE(
		benchrunner::ParseBenchmarkReport(
			"# mono bench report v1\n"
			"bench\ttools.benchrunner.report\t17\t2\t3\t8\tcall\tcache hit\n",
			"tools.benchrunner.report",
			measurements,
			error
		)
	);
	REQUIRE(measurements.size() == 1);
	CHECK(measurements[0].Nanoseconds == 17);
	CHECK(measurements[0].Spread == 2);
	CHECK(measurements[0].Iterations == 8);
	CHECK(measurements[0].Unit == "call");
}

TEST_CASE("benchmark runner refuses an empty selected-suite report", "[benchrunner]") {
	std::vector<benchrunner::Measurement> measurements;
	std::string error;
	CHECK_FALSE(
		benchrunner::ParseBenchmarkReport("# mono bench report v1\n", "missing", measurements, error)
	);
	CHECK(error == "benchmark suite emitted no benchmark rows");
	CHECK(measurements.empty());
}

TEST_CASE("benchmark runner refuses malformed selected-suite rows", "[benchrunner]") {
	std::vector<benchrunner::Measurement> measurements;
	std::string error;
	CHECK_FALSE(
		benchrunner::ParseBenchmarkReport(
			"bench\ttools.benchrunner.report\tno-time\t0\t1\t1\tcall\tbroken\n",
			"tools.benchrunner.report",
			measurements,
			error
		)
	);
	CHECK(error.starts_with("malformed benchmark row:"));
	CHECK(measurements.empty());
}

TEST_CASE("benchmark runner preserves prior measurements when a later row is malformed", "[benchrunner]") {
	std::vector<benchrunner::Measurement> measurements{
		{.Suite = "earlier", .Name = "kept", .Nanoseconds = 1}
	};
	std::string error;
	CHECK_FALSE(
		benchrunner::ParseBenchmarkReport(
			"bench\ttools.benchrunner.report\t17\t2\t3\t8\tcall\tvalid first\n"
			"bench\ttools.benchrunner.report\tbroken\t0\t1\t1\tcall\tinvalid second\n",
			"tools.benchrunner.report",
			measurements,
			error
		)
	);
	CHECK(error.starts_with("malformed benchmark row:"));
	REQUIRE(measurements.size() == 1);
	CHECK(measurements.front().Suite == "earlier");
	CHECK(measurements.front().Name == "kept");
}
