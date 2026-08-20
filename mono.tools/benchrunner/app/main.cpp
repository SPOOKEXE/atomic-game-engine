// The selective benchmark runner.
//
// **The same runner, renamed.** Discovery, the cascading signature and the skip
// cache are `Tool::testrunner`'s and are used here unchanged - that is the shim
// the roadmap asks for, and it is the right shape for one reason: a second
// implementation of "what could this change have affected" is a second thing to
// keep correct, and the neglected one would be the one that silently stopped
// re-running. What differs is only what a run *produces*: a benchmark answers
// "how long", not "did it hold", so there is a baseline instead of a pass and a
// delta instead of a tick.
//
// **Selection matters more here than for tests.** A test suite costs
// milliseconds; a benchmark suite costs seconds by design, because a
// measurement needs samples. Running every one of them on every change is how a
// benchmark suite stops being run at all.
//
// **A regression is reported, never enforced.** This does not fail a build on a
// slow number and it should not: a laptop on battery, a CI runner with a noisy
// neighbour and a desktop with a compile going all produce swings larger than
// most real regressions. The number and its delta go where a person can see
// them, and a person decides. `docs/CODE_QUALITY.md`'s rule about attaching a
// number to an algorithm change is what this exists to serve.

#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <testrunner/Process.hpp>
#include <testrunner/Report.hpp>
#include <testrunner/Runner.hpp>
#include <vector>

namespace fs = std::filesystem;
using namespace testrunner;

namespace {

	// The baseline file's header. Refused rather than read when it does not
	// match, for the same reason the test cache is: numbers gathered
	// differently do not mean the same thing.
	constexpr const char *BASELINE_HEADER = "# mono bench baseline v1";

	// How far a measurement may move before it is worth pointing at.
	//
	// Five per cent, because below that the run-to-run spread of an ordinary
	// machine swallows the signal and every run would report something.
	constexpr double NOTABLE = 0.05;

	// One measured benchmark.
	struct Measurement {
		std::string Suite;
		std::string Name;
		uint64_t Nanoseconds = 0;
		uint64_t Spread = 0;
		uint64_t Iterations = 0;

		// What one iteration is - `call` or `item`. See `BenchUnit`.
		std::string Unit;
	};

	std::vector<std::string> Split(const std::string &text, char separator) {
		std::vector<std::string> fields;
		std::string field;
		std::istringstream stream(text);
		while (std::getline(stream, field, separator)) {
			fields.push_back(field);
		}
		return fields;
	}

	// A benchmark's identity across runs: its suite and its name. Not its
	// position, which changes the moment somebody adds one above it.
	std::string KeyOf(const Measurement &measurement) {
		return measurement.Suite + "\t" + measurement.Name;
	}

	std::map<std::string, uint64_t> LoadBaseline(const fs::path &path) {
		std::map<std::string, uint64_t> baseline;

		std::ifstream file(path);
		if (!file) {
			return baseline;
		}

		std::string line;
		if (!std::getline(file, line) || line != BASELINE_HEADER) {
			return baseline;
		}

		while (std::getline(file, line)) {
			const auto fields = Split(line, '\t');
			if (fields.size() < 3) {
				continue;
			}
			baseline[fields[0] + "\t" + fields[1]] = std::strtoull(fields[2].c_str(), nullptr, 10);
		}
		return baseline;
	}

	bool SaveBaseline(const fs::path &path, const std::map<std::string, uint64_t> &baseline) {
		std::error_code error;
		fs::create_directories(path.parent_path(), error);

		std::ofstream file(path, std::ios::trunc);
		if (!file) {
			return false;
		}

		file << BASELINE_HEADER << '\n';
		// A std::map, so the file is in key order and two runs produce files
		// that can be diffed rather than files that differ everywhere.
		for (const auto &[key, nanoseconds] : baseline) {
			file << key << '\t' << nanoseconds << '\n';
		}
		return static_cast<bool>(file);
	}

	// Runs one suite's benchmarks and parses what the binary printed.
	bool RunSuite(const Suite &suite, int samples, std::vector<Measurement> &into, std::string &output) {
		const auto result =
			Run({suite.Binary.string(), "--suite", suite.Id, "--samples", std::to_string(samples)});

		output = result.Output;
		if (!result.Started || result.ExitCode != 0) {
			return false;
		}

		std::istringstream lines(result.Output);
		std::string line;
		while (std::getline(lines, line)) {
			const auto fields = Split(line, '\t');
			// bench <TAB> suite <TAB> ns <TAB> spread <TAB> samples <TAB> iterations <TAB> unit <TAB> name
			if (fields.size() < 8 || fields[0] != "bench") {
				continue;
			}

			Measurement measurement;
			measurement.Suite = fields[1];
			measurement.Nanoseconds = std::strtoull(fields[2].c_str(), nullptr, 10);
			measurement.Spread = std::strtoull(fields[3].c_str(), nullptr, 10);
			measurement.Iterations = std::strtoull(fields[5].c_str(), nullptr, 10);
			measurement.Unit = fields[6];
			measurement.Name = fields[7];
			into.push_back(std::move(measurement));
		}
		return true;
	}

	// Nanoseconds, in whatever unit reads without counting zeroes.
	std::string FormatTime(uint64_t nanoseconds) {
		std::ostringstream text;
		text << std::fixed << std::setprecision(2);

		if (nanoseconds < 1'000) {
			text << nanoseconds << " ns";
		} else if (nanoseconds < 1'000'000) {
			text << static_cast<double>(nanoseconds) / 1'000.0 << " us";
		} else if (nanoseconds < 1'000'000'000) {
			text << static_cast<double>(nanoseconds) / 1'000'000.0 << " ms";
		} else {
			text << static_cast<double>(nanoseconds) / 1'000'000'000.0 << " s";
		}
		return text.str();
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"benchrunner", "Runs the benchmark suites a change could have affected."
	);

	arguments.Value("build", "DIR", "A configured build directory");
	arguments.Value("cache", "PATH", "Skip cache (default <build>/smart-bench.txt)");
	arguments.Value("baseline", "PATH", "Baseline timings (default .cache/bench-baseline.tsv)");
	// The baseline stays outside the build tree on purpose: it is the record a
	// person compares against across rebuilds, and a file inside `build/` is a
	// file `just clean` throws away. It is only meaningful within one preset,
	// which is why `just bench` always uses the same one.
	arguments.Value("samples", "N", "Samples per benchmark (default 7)");
	arguments.Value("filter", "TEXT", "Only suites whose id contains this");
	arguments.Flag("all", "Run every suite, cache or not");
	arguments.Flag("list", "List suites and signatures, run nothing");
	arguments.Flag("accept", "Write what was measured as the new baseline");

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

	// Inside the build directory, unlike the test runner's - and this is the
	// one place the two deliberately differ. A signature covers sources, not
	// compiler flags, so the same benchmark built at -O0 and at -O2 signs
	// identically and measures nothing like the same. A cache beside the
	// binaries cannot confuse the two; one in `.cache/` would report a debug
	// build's number as an optimised build's and call it unchanged.
	const fs::path cachePath = arguments.Get("cache") ? fs::path(std::string(*arguments.Get("cache")))
													  : buildDirectory / "smart-bench.txt";
	const fs::path baselinePath = arguments.Get("baseline")
									  ? fs::path(std::string(*arguments.Get("baseline")))
									  : fs::path(".cache/bench-baseline.tsv");

	const auto samples = static_cast<int>(arguments.GetInteger("samples", 7));
	const std::string filter =
		arguments.Get("filter") ? std::string(*arguments.Get("filter")) : std::string();

	// The same discovery as the test runner, over a different directory. See
	// the note at the top of this file on why that is one function and not two.
	std::vector<Suite> suites;
	for (const auto &binary : FindBinaries(buildDirectory, "bench")) {
		for (auto &suite : ReadSuites(binary)) {
			if (filter.empty() || suite.Id.find(filter) != std::string::npos) {
				suites.push_back(std::move(suite));
			}
		}
	}

	if (suites.empty()) {
		std::fprintf(
			stderr,
			"no benchmark suites found under %s/bench.\n"
			"Configure with -DMONO_BUILD_BENCH=ON and build first.\n",
			buildDirectory.string().c_str()
		);
		return 1;
	}

	std::vector<std::string> warnings;
	const auto closures = ReadDependencyClosures(buildDirectory);
	const auto signatures = ComputeSignatures(suites, closures, warnings);

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
	auto baseline = LoadBaseline(baselinePath);

	std::vector<const Suite *> stale;
	size_t skipped = 0;
	for (const auto &suite : suites) {
		const auto previous = cache.find(suite.Id);
		const auto &signature = signatures.at(suite.Id);

		const bool unseen = previous == cache.end();
		const bool changed = !unseen && previous->second.Signature != signature;

		// No "was failing" case. A benchmark does not fail, so the only reason
		// to re-run an unchanged one is that somebody asked with `--all`.
		if (arguments.Has("all") || unseen || changed) {
			stale.push_back(&suite);
		} else {
			skipped++;
		}
	}

	std::cout << stale.size() << " suite(s) to measure, " << skipped << " unchanged\n\n";

	std::vector<Measurement> measured;
	std::vector<std::string> broken;

	for (const auto *suite : stale) {
		std::string output;
		if (!RunSuite(*suite, samples, measured, output)) {
			broken.push_back(suite->Id);
			std::cerr << "  FAIL " << suite->Id << '\n' << output << '\n';
			continue;
		}

		CacheEntry entry;
		entry.Signature = signatures.at(suite->Id);
		entry.Passed = true;
		cache[suite->Id] = std::move(entry);
	}

	// Widest name first, so the numbers beside them are a column. A ragged
	// right edge is a list of figures nobody compares.
	size_t widest = 0;
	for (const auto &measurement : measured) {
		widest = std::max(widest, measurement.Name.size());
	}

	std::string suiteShown;
	size_t regressed = 0;
	size_t improved = 0;

	for (const auto &measurement : measured) {
		if (measurement.Suite != suiteShown) {
			suiteShown = measurement.Suite;
			std::cout << suiteShown << '\n';
		}

		std::ostringstream row;
		row << "  " << measurement.Name << std::string(widest - measurement.Name.size() + 2, ' ')
			<< std::setw(12) << std::right << FormatTime(measurement.Nanoseconds);

		// The spread, as a percentage of the figure beside it.
		//
		// **Shown always, because a delta means nothing without it.** A
		// benchmark whose own samples vary by half cannot report a quarter as a
		// change, and the run that taught this lesson reported `+52.1% slower`
		// for code that had just been made faster. A reader who can see the
		// spread can see that for themselves; one who cannot will believe the
		// delta.
		const double noise = measurement.Nanoseconds > 0 ? static_cast<double>(measurement.Spread) /
															   static_cast<double>(measurement.Nanoseconds)
														 : 0.0;
		{
			std::ostringstream spread;
			spread << "+-" << std::fixed << std::setprecision(0) << (noise * 100.0) << "%";
			row << " " << std::setw(7) << std::right << spread.str();
		}

		const auto previous = baseline.find(KeyOf(measurement));
		if (previous != baseline.end() && previous->second > 0) {
			const double delta =
				(static_cast<double>(measurement.Nanoseconds) - static_cast<double>(previous->second)) /
				static_cast<double>(previous->second);

			std::ostringstream change;
			change << std::showpos << std::fixed << std::setprecision(1) << (delta * 100.0) << "%";

			row << "   " << std::setw(8) << std::right << change.str();

			// A move smaller than this run's own spread is not a result. Saying
			// so is the difference between a tool that measures and one that
			// generates confident noise - and the counters below only count
			// what survived the test, so "2 slower" means two that moved
			// further than the measurement could explain.
			if (std::abs(delta) <= noise) {
				row << "  (noise)";
			} else if (delta > NOTABLE) {
				row << "  slower";
				regressed++;
			} else if (delta < -NOTABLE) {
				row << "  faster";
				improved++;
			}
		} else {
			row << "        new";
		}

		std::cout << row.str() << '\n';
	}

	// A benchmark that no longer exists must not linger, or reintroducing one
	// later compares it against a number from a different implementation.
	for (auto entry = cache.begin(); entry != cache.end();) {
		const bool live = signatures.find(entry->first) != signatures.end();
		entry = live ? std::next(entry) : cache.erase(entry);
	}

	if (!SaveCache(cachePath, cache)) {
		std::cerr << "warning: could not write " << cachePath.string() << '\n';
	}

	if (arguments.Has("accept")) {
		// A benchmark that was deleted must not keep its row, or reintroducing
		// the name later compares it against a number from a different
		// implementation. Pruned per *suite* rather than globally: only a suite
		// that actually ran can be authoritative about what it still contains,
		// and a filtered run must not delete what it was told to skip.
		std::vector<std::string> ran;
		for (const auto &measurement : measured) {
			if (std::find(ran.begin(), ran.end(), measurement.Suite) == ran.end()) {
				ran.push_back(measurement.Suite);
			}
		}

		for (auto entry = baseline.begin(); entry != baseline.end();) {
			const std::string suite = entry->first.substr(0, entry->first.find('\t'));
			const bool measuredHere = std::find(ran.begin(), ran.end(), suite) != ran.end();

			const bool stillThere =
				std::any_of(measured.begin(), measured.end(), [&entry](const Measurement &measurement) {
					return KeyOf(measurement) == entry->first;
				});

			entry = (measuredHere && !stillThere) ? baseline.erase(entry) : std::next(entry);
		}

		for (const auto &measurement : measured) {
			baseline[KeyOf(measurement)] = measurement.Nanoseconds;
		}
		if (SaveBaseline(baselinePath, baseline)) {
			std::cout << "\nbaseline written to " << baselinePath.string() << '\n';
		} else {
			std::cerr << "warning: could not write " << baselinePath.string() << '\n';
		}
	} else if (!measured.empty()) {
		std::cout << "\n" << measured.size() << " measured";
		if (regressed > 0 || improved > 0) {
			std::cout << " · " << improved << " faster · " << regressed << " slower";
		}
		std::cout << "\nRe-run with --accept to make these the baseline.\n";
	}

	// **Non-zero only for a benchmark that would not run.** A slow number is a
	// number, not a failure: a laptop on battery and a CI runner with a noisy
	// neighbour both produce swings larger than most real regressions, and a
	// build that went red on one is a build people learn to ignore.
	return broken.empty() ? 0 : 1;
}
