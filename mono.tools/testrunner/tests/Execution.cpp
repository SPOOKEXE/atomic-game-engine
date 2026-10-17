#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <testrunner/Report.hpp>
#include <testrunner/Runner.hpp>
#include <thread>

TEST_SUITE_ID("tools.testrunner.execution")

namespace fs = std::filesystem;

TEST_CASE("parallel discovery preserves complete registries and binary order", "[execution]") {
	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	const fs::path build = fs::current_path();
	const auto registry = testrunner::ReadSuites(self, build);
	REQUIRE_FALSE(registry.empty());
	const fs::path absoluteSelf = fs::canonical(self);
	const std::vector<fs::path> binaries{self, "/nonexistent/test-binary", absoluteSelf};
	for (const size_t jobs : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
		const auto discovered = testrunner::ReadSuites(binaries, build, jobs);
		REQUIRE(discovered.size() == registry.size() * 2);
		for (size_t index = 0; index < discovered.size(); index++) {
			const auto &expected = registry[index % registry.size()];
			CHECK(discovered[index].Id == expected.Id);
			CHECK(discovered[index].Source == expected.Source);
			CHECK(discovered[index].Depends == expected.Depends);
			CHECK(discovered[index].Binary == (index < registry.size() ? self : absoluteSelf));
		}
	}
	CHECK(testrunner::ReadSuites(std::vector<fs::path>{}, build, 8).empty());
}

TEST_CASE("simultaneous runner reports stay inside their own directories", "[execution]") {
	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	struct Outcome {
		fs::path Directory;
		std::string Failure;
		std::string Output;
		testrunner::SuiteReport Report;
		bool Passed = false;
		bool ReportExists = false;
	};
	std::array<Outcome, 2> outcomes;
	std::atomic<unsigned> ready{0};
	std::array<std::thread, 2> workers;
	for (size_t index = 0; index < workers.size(); index++) {
		workers[index] = std::thread([&, index] {
			testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
			Outcome &outcome = outcomes[index];
			outcome.Directory = scratch.Path();
			outcome.Failure = scratch.Failure();
			ready.fetch_add(1);
			while (ready.load() != workers.size())
				std::this_thread::yield();
			if (scratch.Path().empty()) return;

			testrunner::Suite suite;
			suite.Id = "tools.testrunner.report";
			suite.Source = "Report.cpp";
			suite.Binary = self;
			const fs::path path = scratch.Path() / "worker-0.tsv";
			outcome.Passed = testrunner::RunSuite(suite, path, false, outcome.Output, outcome.Report);
			outcome.ReportExists = fs::exists(path);
		});
	}
	for (auto &worker : workers)
		worker.join();

	REQUIRE_FALSE(outcomes[0].Directory.empty());
	REQUIRE_FALSE(outcomes[1].Directory.empty());
	REQUIRE(outcomes[0].Directory != outcomes[1].Directory);
	for (const auto &outcome : outcomes) {
		INFO(outcome.Failure << "\n" << outcome.Output);
		REQUIRE(outcome.Passed);
		REQUIRE(outcome.ReportExists);
		REQUIRE(outcome.Report.CaseCount() > 0);
		REQUIRE_FALSE(fs::exists(outcome.Directory));
	}
}

TEST_CASE("a successful child without a Mono report fails its suite", "[execution]") {
	const fs::path success = fs::exists("/usr/bin/true") ? "/usr/bin/true" : "/bin/true";
	if (!fs::exists(success)) {
		SUCCEED("no true executable on this platform");
		return;
	}

	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	testrunner::Suite suite;
	suite.Source = "Report.cpp";
	suite.Binary = success;
	testrunner::SuiteReport report;
	std::string output;
	REQUIRE_FALSE(testrunner::RunSuite(suite, scratch.Path() / "worker-0.tsv", false, output, report));
	REQUIRE(output.find("without a complete Mono report") != std::string::npos);
	REQUIRE(report.CaseCount() == 0);
}

TEST_CASE("an empty Catch2 selection still writes a valid Mono report", "[execution]") {
	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	testrunner::Suite suite;
	suite.Source = "NoSuchTestSource.cpp";
	suite.Binary = self;
	testrunner::SuiteReport report;
	std::string output;
	const bool passed = testrunner::RunSuite(suite, scratch.Path() / "worker-0.tsv", false, output, report);
	INFO(output);
	REQUIRE(passed);
	REQUIRE(report.CaseCount() == 0);
}
