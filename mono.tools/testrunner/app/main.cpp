#include <engine/core/Arguments.hpp>

#include <cstdio>
#include <iostream>
#include <testrunner/Process.hpp>
#include <testrunner/Runner.hpp>

namespace fs = std::filesystem;
using namespace testrunner;

namespace {

	// Catch2 tags every test with `[#<file stem>]`, and a suite is one file, so
	// the filter is exact. Re-running a whole binary would quietly widen the
	// granularity the identifiers promise.
	bool RunSuite(const Suite &suite, std::string &output) {
		const std::string filter = "[#" + suite.Source.stem().string() + "]";
		const auto result = Run({suite.Binary.string(), "-#", filter});

		output = result.Output;
		if (!result.Started) {
			output = "could not start " + suite.Binary.string();
			return false;
		}
		return result.ExitCode == 0;
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments("testrunner", "Runs the test suites a change could have affected.");

	arguments.Value("build", "DIR", "A configured build directory");
	arguments.Value("cache", "PATH", "Cache file (default .cache/smart-tests.txt)");
	arguments.Flag("all", "Run every suite, cache or not");
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

	std::vector<std::string> failed;
	for (const auto *suite : stale) {
		std::string output;
		const bool ok = RunSuite(*suite, output);

		std::cout << (ok ? "  ok   " : "  FAIL ") << suite->Id << '\n';
		if (!ok) {
			failed.push_back(suite->Id);
			std::cout << output << '\n';
		}

		cache[suite->Id] = CacheEntry{signatures.at(suite->Id), ok};
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

	if (!failed.empty()) {
		std::cerr << '\n' << failed.size() << " suite(s) failed:";
		for (const auto &id : failed) {
			std::cerr << ' ' << id;
		}
		std::cerr << '\n';
		return 1;
	}

	std::cout << '\n' << stale.size() << " run, " << skipped.size() << " skipped, 0 failed\n";
	return 0;
}
