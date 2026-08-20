#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <testrunner/Process.hpp>
#include <testrunner/Report.hpp>
#include <testrunner/Runner.hpp>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using namespace testrunner;

namespace {

	// Catch2 tags every test with `[#<file stem>]`, and a suite is one file, so
	// the filter is exact. Re-running a whole binary would quietly widen the
	// granularity the identifiers promise.
	//
	// Two reporters, not one. `console::out=-` keeps the output a person reads
	// when something goes red exactly as it was, and `mono::out=` writes the
	// counts beside it. Catch2 has taken several reporters at once since 3.0, so
	// the report costs nothing that was there before it.
	//
	// @param scratch Where the `mono` reporter writes. Overwritten per suite.
	// @param report  Counts and failing case names, when the binary got far
	//                enough to write them. Left alone otherwise, so a suite that
	//                crashed reports zero cases rather than a stale suite's.
	bool RunSuite(const Suite &suite, const fs::path &scratch, std::string &output, SuiteReport &report) {
		const std::string filter = "[#" + suite.Source.stem().string() + "]";

		std::error_code error;
		fs::remove(scratch, error);

		// Timed from out here, so the number includes starting the process and
		// running its static initialisers. That is what a person waiting on
		// `just test` actually pays; the reporter's per-case times, which do
		// not include it, are the breakdown underneath.
		const auto started = std::chrono::steady_clock::now();
		const auto result = Run(
			{suite.Binary.string(),
			 "-#",
			 filter,
			 "--reporter",
			 "console::out=-",
			 "--reporter",
			 "mono::out=" + scratch.string()}
		);
		report.Microseconds = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
				.count()
		);

		output = result.Output;
		if (!result.Started) {
			output = "could not start " + suite.Binary.string();
			return false;
		}

		ReadSuiteReport(scratch, report);
		return result.ExitCode == 0;
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments("testrunner", "Runs the test suites a change could have affected.");

	arguments.Value("build", "DIR", "A configured build directory");
	arguments.Value("cache", "PATH", "Cache file (default .cache/smart-tests.txt)");
	arguments.Value("report", "DIR", "Where test-output.md/.html go (default .cache)");
	arguments.Flag("no-report", "Write no documents");
	arguments.Flag("all", "Run every suite, cache or not");
	arguments.Value("jobs", "N", "Suites to run at once (default: 2; 1 is one after another)");
	arguments.Flag("list", "List suites and signatures, run nothing");
	arguments.Flag("verbose", "Name every skipped suite");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	const auto build = arguments.Get("build");
	if (!build) {
		std::fprintf(stderr, "--build is required\n\n%s", arguments.Help().c_str());
		return 2;
	}

	const fs::path buildDirectory = fs::absolute(fs::path(*build));
	if (!fs::is_directory(buildDirectory)) {
		std::fprintf(stderr, "not a build directory: %s\n", buildDirectory.string().c_str());
		return 2;
	}

	const fs::path cachePath = arguments.Get("cache") ? fs::path(std::string(*arguments.Get("cache")))
													  : fs::path(".cache/smart-tests.txt");

	// Beside the cache rather than inside the build tree, and for the same
	// reason: it is one document about the repository, not one per preset
	// nobody would think to go looking in. .gitignore already covers .cache/.
	const fs::path reportDirectory =
		arguments.Get("report") ? fs::path(std::string(*arguments.Get("report"))) : fs::path(".cache");

	std::vector<Suite> suites;
	for (const auto &binary : FindTestBinaries(buildDirectory)) {
		for (auto &suite : ReadSuites(binary)) {
			suites.push_back(std::move(suite));
		}
	}

	if (suites.empty()) {
		std::fprintf(
			stderr,
			"no suites found under %s/tests.\n"
			"Build first, and check every test file has a TEST_SUITE_ID.\n",
			buildDirectory.string().c_str()
		);
		return 1;
	}

	std::vector<std::string> warnings;
	const auto closures = ReadDependencyClosures(buildDirectory);
	const auto signatures = ComputeSignatures(suites, closures, warnings);

	// Reported rather than swallowed: every one of these means the cascade is
	// covering less than it claims to.
	for (const auto &warning : warnings) {
		std::cerr << "warning: " << warning << '\n';
	}

	if (arguments.Has("list")) {
		for (const auto &suite : suites) {
			std::cout << signatures.at(suite.Id).substr(0, 12) << "  " << suite.Id << '\n';
		}
		return 0;
	}

	auto cache = LoadCache(cachePath);

	std::vector<const Suite *> stale;
	std::vector<const Suite *> skipped;
	for (const auto &suite : suites) {
		const auto previous = cache.find(suite.Id);
		const auto &signature = signatures.at(suite.Id);

		const bool unseen = previous == cache.end();
		const bool changed = !unseen && previous->second.Signature != signature;
		// Unchanged but red. Re-running it is the only way it goes green.
		const bool wasFailing = !unseen && !previous->second.Passed;

		if (arguments.Has("all") || unseen || changed || wasFailing) {
			stale.push_back(&suite);
		} else {
			skipped.push_back(&suite);
		}
	}

	std::cout << stale.size() << " suite(s) to run, " << skipped.size() << " unchanged\n";
	if (arguments.Has("verbose")) {
		for (const auto *suite : skipped) {
			std::cout << "  skip " << suite->Id << '\n';
		}
	}

	// Not in the report directory: it is working state, and a directory a person
	// opens documents out of should hold documents.
	const fs::path scratch = buildDirectory / "testrunner-report.tsv";

	std::map<std::string, SuiteReport> reports;

	// The identifiers are a column, so the durations beside them are a column
	// too. A ragged right edge is a list of numbers nobody compares.
	size_t widest = 0;
	for (const auto *suite : stale) {
		widest = std::max(widest, suite->Id.size());
	}

	unsigned long long totalMicroseconds = 0;

	// **Several suites at once, because the slowest ones are asleep.**
	// Measured on this tree: the whole run is about ninety seconds of suite time
	// and `server.replication` alone is thirty-five of it, `server.hostmode`
	// another twelve. Both spend almost all of that waiting in real time for a
	// spawned server process to get somewhere - a wait that cannot be shortened
	// without breaking what it is waiting for, and that costs one core nothing
	// while it happens. Running suites beside each other is therefore the only
	// saving available that does not change what any test asserts.
	//
	// **Two by default, and the number is measured rather than chosen.** On this
	// tree, twenty-four threads: serial is 2 m 54 s, `--jobs 2` is 1 m 36 s over
	// three runs with nothing red, and `--jobs 4` is 1 m 11 s but `server.
	// replication` failed once in three. That suite waits in real time for a
	// spawned server to get somewhere, so loading the machine is loading the
	// thing it is waiting for - the deadline slack in `Remote::Wait` covers a
	// busy box and does not cover an arbitrarily busy one.
	//
	// A test runner that goes red under its own concurrency is worse than a slow
	// one, so the default is the fastest setting that was not observed to flake.
	// `--jobs 1` is the old behaviour exactly, and a higher number is there for
	// anybody who would rather have the minute back.
	const int requested = static_cast<int>(arguments.GetNumber("jobs", 2.0));
	const auto jobs = static_cast<size_t>(std::max(requested, 1));

	// **A scratch file each.** The `mono` reporter writes to a path this process
	// hands it, and one shared path is one suite reading another's counts. This
	// is the only shared state the loop had.
	const auto scratchFor = [&buildDirectory](size_t slot) {
		return buildDirectory / ("testrunner-report-" + std::to_string(slot) + ".tsv");
	};

	struct Outcome {
		std::string Output;
		SuiteReport Report;
		bool Ok = false;
	};

	std::vector<Outcome> outcomes(stale.size());
	std::atomic<size_t> next{0};

	{
		std::vector<std::thread> workers;
		const size_t width = std::min(jobs, stale.size());
		workers.reserve(width);

		for (size_t slot = 0; slot < width; slot++) {
			workers.emplace_back([&, slot] {
				const fs::path mine = scratchFor(slot);
				for (size_t index = next++; index < stale.size(); index = next++) {
					const Suite *suite = stale[index];
					Outcome &outcome = outcomes[index];
					outcome.Report.Id = suite->Id;
					outcome.Report.Ran = true;
					outcome.Ok = RunSuite(*suite, mine, outcome.Output, outcome.Report);
					outcome.Report.Passed = outcome.Ok;
				}

				std::error_code error;
				fs::remove(mine, error);
			});
		}

		for (std::thread &worker : workers) {
			worker.join();
		}
	}

	// **Reported in the order the suites were listed, not the order they
	// finished.** A run whose output shuffles between invocations is a run
	// nobody can diff, and the ordering is free: the results are already
	// collected.
	std::vector<std::string> failed;
	for (size_t index = 0; index < stale.size(); index++) {
		const Suite *suite = stale[index];
		Outcome &outcome = outcomes[index];
		totalMicroseconds += outcome.Report.Microseconds;

		std::cout << (outcome.Ok ? "  ok   " : "  FAIL ") << suite->Id
				  << std::string(widest - suite->Id.size() + 2, ' ')
				  << FormatDuration(outcome.Report.Microseconds) << '\n';
		if (!outcome.Ok) {
			failed.push_back(suite->Id);
			std::cout << outcome.Output << '\n';
		}

		CacheEntry entry;
		entry.Signature = signatures.at(suite->Id);
		entry.Passed = outcome.Ok;
		entry.CasesPassed = outcome.Report.CasesPassed;
		entry.CasesFailed = outcome.Report.CasesFailed;
		entry.CasesSkipped = outcome.Report.CasesSkipped;
		entry.AssertionsPassed = outcome.Report.AssertionsPassed;
		entry.AssertionsFailed = outcome.Report.AssertionsFailed;
		entry.Microseconds = outcome.Report.Microseconds;
		cache[suite->Id] = std::move(entry);

		reports[suite->Id] = std::move(outcome.Report);
	}

	std::error_code scratchError;
	fs::remove(scratch, scratchError);

	// A suite that no longer exists must not linger, or reintroducing one later
	// makes it look already-passed.
	for (auto entry = cache.begin(); entry != cache.end();) {
		const bool live = signatures.find(entry->first) != signatures.end();
		entry = live ? std::next(entry) : cache.erase(entry);
	}

	if (!SaveCache(cachePath, cache)) {
		std::cerr << "warning: could not write " << cachePath.string() << '\n';
	}

	// Every suite the runner knows about, not only the ones this invocation
	// chose to run. A report covering only what one change happened to touch
	// would be a report of the change, and the thing worth having is the shape
	// of the whole tree.
	if (!arguments.Has("no-report")) {
		std::vector<SuiteReport> everything;
		everything.reserve(suites.size());

		for (const auto *suite : skipped) {
			const auto cached = cache.find(suite->Id);
			if (cached == cache.end()) {
				continue;
			}

			SuiteReport report;
			report.Id = suite->Id;
			report.Ran = false;
			report.Passed = cached->second.Passed;
			report.CasesPassed = cached->second.CasesPassed;
			report.CasesFailed = cached->second.CasesFailed;
			report.CasesSkipped = cached->second.CasesSkipped;
			report.AssertionsPassed = cached->second.AssertionsPassed;
			report.AssertionsFailed = cached->second.AssertionsFailed;
			// What it cost the last time it ran. Cases stays empty: the cache
			// keeps a suite's total, not its breakdown.
			report.Microseconds = cached->second.Microseconds;
			everything.push_back(std::move(report));
		}

		for (auto &[id, report] : reports) {
			everything.push_back(std::move(report));
		}

		if (WriteReports(reportDirectory, everything)) {
			std::cout << "report  " << (reportDirectory / "test-output.md").string() << '\n';
			std::cout << "report  " << (reportDirectory / "test-output.html").string() << '\n';
		} else {
			std::cerr << "warning: could not write the report into " << reportDirectory.string() << '\n';
		}
	}

	if (!failed.empty()) {
		std::cerr << '\n' << failed.size() << " suite(s) failed:";
		for (const auto &id : failed) {
			std::cerr << ' ' << id;
		}
		std::cerr << '\n';
		return 1;
	}

	// No duration when nothing ran. "0 µs" is true and reads as a broken clock.
	std::cout << '\n' << stale.size() << " run, " << skipped.size() << " skipped, 0 failed";
	if (!stale.empty()) {
		std::cout << " in " << FormatDuration(totalMicroseconds);
	}
	std::cout << '\n';
	return 0;
}
