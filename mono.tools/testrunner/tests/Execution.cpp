#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
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
	const std::vector<fs::path> binaries{self, absoluteSelf};
	for (const size_t jobs : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
		const auto discovered = testrunner::ReadSuites(binaries, build, jobs);
		REQUIRE(discovered.size() == registry.size() * 2);
		for (size_t index = 0; index < discovered.size(); index++) {
			const auto &expected = registry[index % registry.size()];
			CHECK(discovered[index].Id == expected.Id);
			CHECK(discovered[index].Source == expected.Source);
			CHECK(discovered[index].Depends == expected.Depends);
			CHECK(discovered[index].Tier == expected.Tier);
			CHECK(discovered[index].ScenarioGroup == expected.ScenarioGroup);
			CHECK(discovered[index].HasSlowCases == expected.HasSlowCases);
			CHECK(discovered[index].Binary == (index < registry.size() ? self : absoluteSelf));
		}
	}
	CHECK(testrunner::ReadSuites(std::vector<fs::path>{}, build, 8).empty());
}

TEST_CASE("slow discovery metadata is conservative when absent or invalid", "[execution][discovery]") {
	if (!fs::exists("/bin/sh")) return;
	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const auto binary = scratch.Path() / "discovery-child";
	{
		std::ofstream child(binary);
		child << "#!/bin/sh\nprintf 'old\\t/old.cpp\\t\\t\\t\\n'\n"
				 "printf 'empty\\t/empty.cpp\\t\\t\\t\\tslow=0\\n'\n"
				 "printf 'slow\\t/slow.cpp\\t\\t\\t\\tslow=1\\n'\n"
				 "printf 'bad\\t/bad.cpp\\t\\t\\t\\tslow=maybe\\n'\n";
	}
	fs::permissions(binary, fs::perms::owner_all);
	const auto suites = testrunner::ReadSuites(binary, scratch.Path());
	REQUIRE(suites.size() == 4);
	CHECK(suites[0].MayHaveSlowCases());
	CHECK_FALSE(suites[1].MayHaveSlowCases());
	CHECK(suites[2].MayHaveSlowCases());
	CHECK(suites[3].MayHaveSlowCases());
}

TEST_CASE("failed discovery never returns a partial registry", "[execution][discovery]") {
	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const auto missing = scratch.Path() / "missing-binary";
	CHECK_THROWS_AS(testrunner::ReadSuites(missing, scratch.Path()), std::runtime_error);

	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) return;
	for (const size_t jobs : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
		CHECK_THROWS_AS(
			testrunner::ReadSuites(std::vector<fs::path>{self, missing, self}, scratch.Path(), jobs),
			std::runtime_error
		);
	}
}

TEST_CASE(
	"discovery retains failed child diagnostics and accepts an empty registry", "[execution][discovery]"
) {
	if (!fs::exists("/bin/sh")) {
		SUCCEED("no shell fixture on this platform");
		return;
	}
	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const auto binary = scratch.Path() / "discovery-child";
	{
		std::ofstream child(binary);
		child << "#!/bin/sh\nprintf 'partial\\t/partial.cpp\\n'\n"
				 "printf 'test source has no TEST_SUITE_ID: /missing.cpp\\n' >&2\nexit 19\n";
	}
	fs::permissions(binary, fs::perms::owner_all);
	try {
		(void)testrunner::ReadSuites(binary, scratch.Path());
		FAIL("failed discovery returned a registry");
	} catch (const std::runtime_error &error) {
		const std::string message = error.what();
		CHECK(message.find(binary.string()) != std::string::npos);
		CHECK(message.find("exit 19") != std::string::npos);
		CHECK(message.find("TEST_SUITE_ID: /missing.cpp") != std::string::npos);
	}
	{
		std::ofstream child(binary);
		child << "#!/bin/sh\nexit 0\n";
	}
	CHECK(testrunner::ReadSuites(binary, scratch.Path()).empty());
}

TEST_CASE("discovery reads each suite's declared tier", "[execution][tier]") {
	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}

	const auto registry = testrunner::ReadSuites(self, fs::current_path());
	const auto tierOf = [&](std::string_view id) -> std::optional<std::string> {
		for (const auto &suite : registry)
			if (suite.Id == id) return suite.Tier;
		return std::nullopt;
	};
	// Cascade.cpp declares TEST_TIER("fast"); this file declares none.
	CHECK(tierOf("tools.testrunner.cascade") == "fast");
	CHECK(tierOf("tools.testrunner.execution") == "");
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
			outcome.Passed = testrunner::RunSuite(
				suite, path, false, testrunner::SlowCases::Include, outcome.Output, outcome.Report
			);
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
	REQUIRE_FALSE(
		testrunner::RunSuite(
			suite, scratch.Path() / "worker-0.tsv", false, testrunner::SlowCases::Include, output, report
		)
	);
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
	const bool passed = testrunner::RunSuite(
		suite, scratch.Path() / "worker-0.tsv", false, testrunner::SlowCases::Include, output, report
	);
	INFO(output);
	REQUIRE(passed);
	REQUIRE(report.CaseCount() == 0);
}

TEST_CASE("scenario groups preserve selection binary and tier boundaries", "[execution][scenario]") {
	testrunner::Suite first;
	first.Id = "first";
	first.Binary = "test-one";
	first.ScenarioGroup = "scene";
	auto second = first;
	second.Id = "second";
	auto isolated = first;
	isolated.ScenarioGroup.clear();
	auto otherBinary = first;
	otherBinary.Binary = "test-two";
	auto otherTier = first;
	otherTier.Tier = "fast";
	const auto groups = testrunner::PlanSuiteGroups({&first, &isolated, &second, &otherBinary, &otherTier});
	REQUIRE(groups.size() == 4);
	CHECK(groups[0] == std::vector<const testrunner::Suite *>{&first, &second});
	CHECK(testrunner::PlanSuiteGroups({&second})[0].size() == 1);
	CHECK(testrunner::PlanSuiteGroups({}).empty());
}

TEST_CASE("a shared child keeps passing and failing source suites distinct", "[execution][scenario]") {
	if (!fs::exists("/bin/sh")) {
		SUCCEED("no shell fixture on this platform");
		return;
	}
	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const auto binary = scratch.Path() / "group-child";
	const auto invocation = scratch.Path() / "invocations";
	const auto firstSource = scratch.Path() / "First.cpp";
	const auto secondSource = scratch.Path() / "Second.cpp";
	{
		std::ofstream script(binary);
		script << "#!/bin/sh\n"
				  "for arg do case \"$arg\" in mono::out=*) report=${arg#mono::out=};; esac; done\n"
				  "echo run >> '"
			   << invocation.string()
			   << "'\n"
				  "printf '# mono test report v3\\nfailure-exit\\t42\\nsource\\t1\\t"
			   << firstSource.string() << "\\nsource\\t1\\t" << secondSource.string()
			   << "\\nmanifest-end\\ncase\\tpass\\t2\\t0\\t10\\t" << firstSource.string()
			   << "\\tfirst\\ncase\\tfail\\t0\\t1\\t20\\t" << secondSource.string()
			   << "\\tsecond\\ntotal\\t1\\t1\\t0\\t2\\t1\\n' > \"$report\"\nexit 42\n";
	}
	fs::permissions(binary, fs::perms::owner_all);
	testrunner::Suite first;
	first.Id = "first";
	first.Source = firstSource;
	first.Binary = binary;
	first.ScenarioGroup = "scene";
	auto second = first;
	second.Id = "second";
	second.Source = secondSource;
	std::string output;
	const auto reports = testrunner::RunSuiteGroup(
		{&first, &second}, scratch.Path() / "report.tsv", false, testrunner::SlowCases::Skip, output
	);
	INFO(output);
	REQUIRE(reports.size() == 2);
	CHECK(reports[0].Passed);
	CHECK_FALSE(reports[1].Passed);
	CHECK(reports[0].AssertionsPassed == 2);
	CHECK(reports[1].AssertionsFailed == 1);
	CHECK(reports[0].Cases.size() == 1);
	CHECK(reports[1].Cases.size() == 1);
	std::ifstream called(invocation);
	std::string line;
	REQUIRE(std::getline(called, line));
	CHECK_FALSE(std::getline(called, line));

	// An unselected source in a child's output invalidates the whole group.
	auto unknown = second;
	unknown.Source = scratch.Path() / "Unknown.cpp";
	const auto invalid = testrunner::RunSuiteGroup(
		{&first, &unknown}, scratch.Path() / "report.tsv", false, testrunner::SlowCases::Skip, output
	);
	CHECK_FALSE(invalid[0].Passed);
	CHECK_FALSE(invalid[1].Passed);
	const auto replaceChild = [&](const std::string &manifest, int exitCode) {
		std::ofstream script(binary);
		script << "#!/bin/sh\n"
				  "for arg do case \"$arg\" in mono::out=*) report=${arg#mono::out=};; esac; done\n"
				  "cat > \"$report\" <<'MONO'\n# mono test report v3\nfailure-exit\t42\n"
			   << manifest << "manifest-end\ncase\tpass\t2\t0\t10\t" << firstSource.string()
			   << "\tfirst\ntotal\t1\t0\t0\t2\t0\nMONO\nexit " << exitCode << '\n';
	};
	const std::string firstManifest = "source\t1\t" + firstSource.string() + "\n";
	for (const std::string &manifest :
		 {firstManifest, firstManifest + "source\t1\t" + secondSource.string() + "\n"}) {
		replaceChild(manifest, 0);
		const auto omitted = testrunner::RunSuiteGroup(
			{&first, &second}, scratch.Path() / "report.tsv", false, testrunner::SlowCases::Skip, output
		);
		INFO(output);
		CHECK_FALSE(omitted[0].Passed);
		CHECK_FALSE(omitted[1].Passed);
	}
	// A declared zero distinguishes a filtered peer from an omitted eligible peer.
	replaceChild(firstManifest + "source\t0\t" + secondSource.string() + "\n", 0);
	const auto filtered = testrunner::RunSuiteGroup(
		{&first, &second}, scratch.Path() / "report.tsv", false, testrunner::SlowCases::Skip, output
	);
	INFO(output);
	CHECK(filtered[0].Passed);
	CHECK(filtered[1].Passed);
	CHECK(filtered[1].CaseCount() == 0);
	replaceChild(firstManifest + "source\t0\t" + secondSource.string() + "\n", 42);
	const auto unexpectedExit = testrunner::RunSuiteGroup(
		{&first, &second}, scratch.Path() / "report.tsv", false, testrunner::SlowCases::Skip, output
	);
	CHECK_FALSE(unexpectedExit[0].Passed);
	CHECK_FALSE(unexpectedExit[1].Passed);
}

TEST_CASE("real Catch grouped reports retain selected source ownership", "[execution][scenario]") {
	const fs::path self = "/proc/self/exe";
	if (!fs::exists(self)) {
		SUCCEED("no /proc/self/exe on this platform");
		return;
	}
	const auto registry = testrunner::ReadSuites(self, fs::current_path());
	std::vector<testrunner::Suite> selected;
	for (const auto &suite : registry) {
		if (suite.Id != "tools.testrunner.report" && suite.Id != "tools.testrunner.sha256") continue;
		selected.push_back(suite);
		selected.back().ScenarioGroup = "runner-proof";
	}
	REQUIRE(selected.size() == 2);
	testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	std::string output;
	const auto reports = testrunner::RunSuiteGroup(
		{&selected[0], &selected[1]}, scratch.Path() / "group.tsv", false, testrunner::SlowCases::Skip, output
	);
	INFO(output);
	REQUIRE(reports.size() == 2);
	for (size_t index = 0; index < reports.size(); index++) {
		CHECK(reports[index].Passed);
		REQUIRE_FALSE(reports[index].Cases.empty());
		for (const auto &entry : reports[index].Cases)
			CHECK(fs::weakly_canonical(entry.Source) == selected[index].Source);
	}
	const auto partial = testrunner::RunSuiteGroup(
		{&selected[0]}, scratch.Path() / "partial.tsv", false, testrunner::SlowCases::Skip, output
	);
	INFO(output);
	REQUIRE(partial.size() == 1);
	CHECK(partial[0].Passed);
	CHECK(partial[0].CaseCount() == reports[0].CaseCount());
}
