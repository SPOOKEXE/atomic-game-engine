// The main() every benchmark binary links.
//
// Two jobs, and no framework. `--mono-suites` prints what the binary contains
// so mono.tools/benchrunner can sign and select it, exactly as a test binary
// does; anything else runs the benchmarks and prints one tab-separated line
// each. There is no Catch2 here: a benchmark measures, it does not assert, and
// linking a test framework for a timing loop would mean the framework's own
// per-case overhead landed inside the numbers.
//
//     # mono bench report v1
//     bench<TAB>suite<TAB>nanoseconds<TAB>spread<TAB>samples<TAB>iterations<TAB>unit<TAB>name
//
// `nanoseconds` is the *minimum* sample, per iteration. See `Bench.hpp` on why
// that is the right statistic and the mean is not.

#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace engine::testing {

	// Function-local, so a benchmark declared during static initialisation in
	// another translation unit cannot race the container's construction.
	static std::vector<BenchCase> &Cases() {
		static std::vector<BenchCase> cases;
		return cases;
	}

	void BenchRegistry::Declare(BenchCase entry) {
		Cases().push_back(std::move(entry));
	}

	const std::vector<BenchCase> &BenchRegistry::All() {
		return Cases();
	}
}

namespace {

	// How many times each benchmark is measured, unless told otherwise.
	//
	// Small, because the statistic taken is the minimum and a minimum converges
	// quickly - the samples above it are the machine's noise and collecting
	// more of them buys precision about the noise.
	constexpr int DEFAULT_SAMPLES = 7;

	// Samples run before any are kept.
	//
	// The first run of anything pays for cold caches, lazy page faults and a
	// branch predictor that has never seen the code. Including that would be
	// reporting the allocator rather than the algorithm.
	constexpr int WARMUP_SAMPLES = 2;

	// A record is one line and its free-text field is last, so a name carrying
	// a tab would otherwise end the record early.
	std::string OneLine(std::string_view text) {
		std::string flattened(text);
		for (char &character : flattened) {
			if (character == '\t' || character == '\n' || character == '\r') {
				character = ' ';
			}
		}
		return flattened;
	}

	// One sample: the whole body, `Iterations` times.
	uint64_t Sample(const engine::testing::BenchCase &entry) {
		const auto started = std::chrono::steady_clock::now();
		entry.Body();
		const auto ended = std::chrono::steady_clock::now();
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
		);
	}
}

int main(int argc, char **argv) {
	int samples = DEFAULT_SAMPLES;
	std::string filter;

	for (int index = 1; index < argc; index++) {
		if (std::strcmp(argv[index], "--mono-suites") == 0) {
			for (const auto &suite : engine::testing::Registry::All()) {
				std::cout << suite.Id << '\t' << suite.File << '\t';
				for (size_t depth = 0; depth < suite.Depends.size(); depth++) {
					if (depth > 0) {
						std::cout << ',';
					}
					std::cout << suite.Depends[depth];
				}
				std::cout << '\n';
			}
			return 0;
		}
		if (std::strcmp(argv[index], "--samples") == 0 && index + 1 < argc) {
			samples = std::max(1, std::atoi(argv[++index]));
			continue;
		}
		if (std::strcmp(argv[index], "--suite") == 0 && index + 1 < argc) {
			// One suite at a time, so the runner's selection is honoured and a
			// benchmark that did not change is not paid for.
			filter = argv[++index];
			continue;
		}
	}

	std::cout << "# mono bench report v1\n";

	for (const auto &entry : engine::testing::BenchRegistry::All()) {
		if (!filter.empty() && entry.Suite != filter) {
			continue;
		}

		for (int warm = 0; warm < WARMUP_SAMPLES; warm++) {
			Sample(entry);
		}

		uint64_t fastest = UINT64_MAX;
		uint64_t slowest = 0;
		for (int taken = 0; taken < samples; taken++) {
			const uint64_t elapsed = Sample(entry);
			fastest = std::min(fastest, elapsed);
			slowest = std::max(slowest, elapsed);
		}

		const auto iterations = static_cast<uint64_t>(std::max<size_t>(1, entry.Iterations));

		// Per iteration, so two benchmarks with different loop counts are
		// comparable and a body that got faster shows as a smaller number.
		// Integer division: a decimal here would be at the mercy of whatever
		// locale the binary started in, and the runner splits on tabs.
		// **The unit goes before the name and not after it.** The name is free
		// text flattened onto one line, so anything past it cannot be found by
		// counting tabs.
		std::cout << "bench\t" << entry.Suite << '\t' << (fastest / iterations) << '\t'
				  << ((slowest - fastest) / iterations) << '\t' << samples << '\t' << iterations << '\t'
				  << engine::testing::Describe(entry.Unit) << '\t' << OneLine(entry.Name) << '\n';
	}

	std::cout.flush();
	return 0;
}
