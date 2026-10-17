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
//     # mono bench latency
//     v1<TAB>suite<TAB>samples<TAB>iterations<TAB>unit<TAB>p50<TAB>p95<TAB>p99<TAB>max<TAB>name
//
// `nanoseconds` is the *minimum* sample, per iteration. See `Bench.hpp` on why
// that is the right statistic and the mean is not.
// Set MONO_BENCH_LATENCY=1 for the optional comment row, which uses only measured samples.

#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
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
	// Two samples did not settle the physics broadphase after a low-work row:
	// its first seven measured samples varied by 34 percent while later samples
	// stayed within 3 percent. Eight warms the code, data and CPU frequency before
	// the usual seven-sample minimum is allowed to describe the algorithm.
	constexpr int WARMUP_SAMPLES = 8;
	constexpr int MAXIMUM_WARMUP_SAMPLES = 64;
	constexpr int MAXIMUM_LATENCY_SAMPLES = 10'000;

	bool ParseWarmupSamples(std::string_view text, int &out) {
		if (text.empty()) return false;
		for (const char character : text) {
			if (character < '0' || character > '9') return false;
		}

		unsigned value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size() || value > MAXIMUM_WARMUP_SAMPLES) {
			return false;
		}
		out = static_cast<int>(value);
		return true;
	}

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

	void PrintHelp() {
		std::cout << "Usage: bench [options]\n"
				  << "Options:\n"
				  << "  -h, --help                 Show this help and exit.\n"
				  << "  --mono-suites              List suites for discovery and exit.\n"
				  << "  --suite ID                 Run only benchmarks in ID.\n"
				  << "  --benchmark NAME           Run benchmarks with this exact name.\n"
				  << "  --samples N                Measured samples per benchmark (default: "
				  << DEFAULT_SAMPLES << ").\n"
				  << "  --warmup-samples N         Harness warmups, 0 through " << MAXIMUM_WARMUP_SAMPLES
				  << " (default: " << WARMUP_SAMPLES << ").\n";
	}
}

int main(int argc, char **argv) {
	int samples = DEFAULT_SAMPLES;
	int warmupSamples = WARMUP_SAMPLES;
	std::string suiteFilter;
	std::string benchmarkFilter;
	bool hasBenchmarkFilter = false;
	const char *latencySetting = std::getenv("MONO_BENCH_LATENCY");
	const bool reportLatency = latencySetting != nullptr && std::strcmp(latencySetting, "1") == 0;

	for (int index = 1; index < argc; index++) {
		if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0) {
			PrintHelp();
			return 0;
		}
	}

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
		if (std::strcmp(argv[index], "--warmup-samples") == 0) {
			if (index + 1 >= argc) {
				std::cerr << "--warmup-samples requires a decimal integer from 0 through "
						  << MAXIMUM_WARMUP_SAMPLES << '\n';
				return 2;
			}
			if (!ParseWarmupSamples(argv[++index], warmupSamples)) {
				std::cerr << "--warmup-samples must be a decimal integer from 0 through "
						  << MAXIMUM_WARMUP_SAMPLES << '\n';
				return 2;
			}
			continue;
		}
		if (std::strcmp(argv[index], "--suite") == 0 && index + 1 < argc) {
			// One suite at a time, so the runner's selection is honoured and a
			// benchmark that did not change is not paid for.
			suiteFilter = argv[++index];
			continue;
		}
		if (std::strcmp(argv[index], "--benchmark") == 0) {
			if (index + 1 >= argc) {
				std::cerr << "--benchmark requires a non-empty exact name\n";
				return 2;
			}
			benchmarkFilter = argv[++index];
			if (benchmarkFilter.empty()) {
				std::cerr << "--benchmark requires a non-empty exact name\n";
				return 2;
			}
			hasBenchmarkFilter = true;
			continue;
		}
	}
	const auto selected = [&](const engine::testing::BenchCase &entry) {
		return (suiteFilter.empty() || entry.Suite == suiteFilter) &&
			   (!hasBenchmarkFilter || entry.Name == benchmarkFilter);
	};
	if (hasBenchmarkFilter && std::none_of(
								  engine::testing::BenchRegistry::All().begin(),
								  engine::testing::BenchRegistry::All().end(),
								  selected
							  )) {
		std::cerr << "no benchmark matches the requested selection\n";
		return 2;
	}
	if (reportLatency && samples > MAXIMUM_LATENCY_SAMPLES) {
		std::cerr << "latency reporting accepts at most " << MAXIMUM_LATENCY_SAMPLES << " samples\n";
		return 2;
	}

	std::cout << "# mono bench report v1\n";
	std::cout << "# mono bench warmup v1\t" << warmupSamples << '\n';

	for (const auto &entry : engine::testing::BenchRegistry::All()) {
		if (!selected(entry)) {
			continue;
		}

		for (int warm = 0; warm < warmupSamples; warm++) {
			Sample(entry);
		}

		uint64_t fastest = UINT64_MAX;
		uint64_t slowest = 0;
		std::vector<uint64_t> measured;
		if (reportLatency) measured.reserve(static_cast<size_t>(samples));
		for (int taken = 0; taken < samples; taken++) {
			const uint64_t elapsed = Sample(entry);
			fastest = std::min(fastest, elapsed);
			slowest = std::max(slowest, elapsed);
			if (reportLatency) measured.push_back(elapsed);
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
		if (reportLatency) {
			std::sort(measured.begin(), measured.end());
			const auto percentile = [&](size_t numerator) {
				const size_t rank = (measured.size() * numerator + 99) / 100;
				return measured[rank - 1] / iterations;
			};
			// Comment rows leave the v1 parser intact. Samples are complete body
			// durations, normalized like the ordinary row, after warmup only.
			std::cout << "# mono bench latency v1\t" << entry.Suite << '\t' << samples << '\t' << iterations
					  << '\t' << engine::testing::Describe(entry.Unit) << '\t' << percentile(50) << '\t'
					  << percentile(95) << '\t' << percentile(99) << '\t' << (measured.back() / iterations)
					  << '\t' << OneLine(entry.Name) << '\n';
		}
	}

	std::cout.flush();
	return 0;
}
