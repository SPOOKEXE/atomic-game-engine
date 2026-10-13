#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <testrunner/Report.hpp>
#include <testrunner/Runner.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace testrunner;

int main(int argc, char **argv) {
	engine::core::Arguments arguments("testrunner", "Runs the test suites a change could have affected.");

	arguments.Value("build", "DIR", "A configured build directory");
	arguments.Value("cache", "PATH", "Cache file (default .cache/smart-tests.txt)");
	arguments.Value("report", "DIR", "Where test-output.md/.html go (default .cache)");
	arguments.Flag("no-report", "Write no documents");
	arguments.Flag("all", "Run every suite, cache or not");
	arguments.Flag("gpu-tests", "Run every suite, including headless GPU cases");
	arguments.Value("jobs", "N", "Discovery and suite workers (default: 8; 1 is one after another)");
	arguments.Flag("list", "List suites and signatures, run nothing");
	arguments.Flag("verbose", "Name every skipped suite");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.VersionRequested) {
		std::fputs(arguments.VersionLine().c_str(), stdout);
		return 0;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
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

	const int requested = static_cast<int>(arguments.GetNumber("jobs", 8.0));
	const auto jobs = static_cast<size_t>(std::max(requested, 1));
	const auto suites = ReadSuites(FindTestBinaries(buildDirectory), buildDirectory, jobs);

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

		if (arguments.Has("all") || arguments.Has("gpu-tests") || unseen || changed || wasFailing) {
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
	std::cout.flush();

	// A separate invocation can run against the same build tree. Reserve this
	// directory before any worker starts, so only this invocation can remove it.
	ReportScratchDirectory scratch(buildDirectory);
	if (scratch.Path().empty()) {
		std::cerr << "could not create test report scratch directory under " << buildDirectory.string()
				  << ": " << scratch.Failure() << '\n';
		return 1;
	}

	std::map<std::string, SuiteReport> reports;
	unsigned long long totalMicroseconds = 0;

	// Cached durations put known long suites first, reducing the final idle tail.
	// Unknown and equal-cost suites retain their discovery order.
	std::stable_sort(stale.begin(), stale.end(), [&](const Suite *left, const Suite *right) {
		const auto duration = [&](const Suite *suite) {
			const auto previous = cache.find(suite->Id);
			return previous == cache.end() ? 0ULL : previous->second.Microseconds;
		};
		return duration(left) > duration(right);
	});

	// Each worker reuses its slot only within this invocation's directory.
	const auto scratchFor = [&scratch](size_t slot) {
		return scratch.Path() / ("worker-" + std::to_string(slot) + ".tsv");
	};

	struct Outcome {
		std::string Output;
		SuiteReport Report;
		bool Ok = false;
	};

	std::vector<Outcome> outcomes(stale.size());
	std::atomic<size_t> next{0};
	const size_t width = std::min(jobs, stale.size());

	struct ProgressEvent {
		size_t Slot;
		size_t Index;
		bool Started;
		std::chrono::steady_clock::time_point At;
	};
	struct RunningSuite {
		const Suite *Current = nullptr;
		std::chrono::steady_clock::time_point Started;
	};
	std::mutex progressMutex;
	std::condition_variable progressChanged;
	std::vector<ProgressEvent> pending;
	pending.reserve(stale.size() * 2);
	std::vector<RunningSuite> running(width);

	// Workers only publish events. The main thread writes every progress line,
	// so a redirected log never interleaves two suite names or a heartbeat.
	const auto publish = [&](size_t slot, size_t index, bool started) {
		{
			std::lock_guard lock(progressMutex);
			pending.push_back({slot, index, started, std::chrono::steady_clock::now()});
		}
		progressChanged.notify_one();
	};

	{
		std::vector<std::thread> workers;
		workers.reserve(width);

		for (size_t slot = 0; slot < width; slot++) {
			workers.emplace_back([&, slot] {
				const fs::path mine = scratchFor(slot);
				for (size_t index = next++; index < stale.size(); index = next++) {
					const Suite *suite = stale[index];
					Outcome &outcome = outcomes[index];
					outcome.Report.Id = suite->Id;
					outcome.Report.Ran = true;
					publish(slot, index, true);
					outcome.Ok =
						RunSuite(*suite, mine, arguments.Has("gpu-tests"), outcome.Output, outcome.Report);
					outcome.Report.Passed = outcome.Ok;
					publish(slot, index, false);
				}
			});
		}

		size_t completed = 0;
		auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (completed < stale.size()) {
			std::unique_lock lock(progressMutex);
			progressChanged.wait_until(lock, nextHeartbeat, [&] { return !pending.empty(); });

			for (const ProgressEvent &event : pending) {
				const Suite &suite = *stale[event.Index];
				if (event.Started) {
					running[event.Slot] = {&suite, event.At};
					std::cout << "  start [" << completed << '/' << stale.size() << "] " << suite.Id << '\n';
				} else {
					running[event.Slot].Current = nullptr;
					completed++;
					const Outcome &outcome = outcomes[event.Index];
					std::cout << (outcome.Ok ? "  ok    [" : "  FAIL  [") << completed << '/' << stale.size()
							  << "] " << suite.Id << "  " << FormatDuration(outcome.Report.Microseconds)
							  << '\n';
				}
			}
			pending.clear();

			const auto now = std::chrono::steady_clock::now();
			if (completed < stale.size() && now >= nextHeartbeat) {
				std::cout << "  heartbeat [" << completed << '/' << stale.size() << "] running:";
				for (const RunningSuite &active : running) {
					if (active.Current == nullptr) continue;
					const auto elapsed =
						std::chrono::duration_cast<std::chrono::seconds>(now - active.Started);
					std::cout << '\n' << "    " << active.Current->Id << "  " << elapsed.count() << " s";
				}
				std::cout << '\n';
				nextHeartbeat = now + std::chrono::seconds(30);
			}
			std::cout.flush();
		}

		for (std::thread &worker : workers) {
			worker.join();
		}
	}

	// Failure details, cache entries and documents stay in suite order. Live
	// progress above already named each passing suite when it completed.
	std::vector<std::string> failed;
	for (size_t index = 0; index < stale.size(); index++) {
		const Suite *suite = stale[index];
		Outcome &outcome = outcomes[index];
		totalMicroseconds += outcome.Report.Microseconds;

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
