#pragma once

// Benchmarks, declared the way tests are.
//
// A benchmark binary is discovered, signed and selected by exactly the
// machinery that selects tests: it declares `TEST_SUITE_ID`, answers
// `--mono-suites`, and gets a cascading signature over its own source and every
// header it includes. That is the "shim and rename" the roadmap asks for — a
// second discovery mechanism would be a second thing to keep correct, and the
// one that got neglected would be the one that silently stopped re-running.
//
//     TEST_SUITE_ID("engine.ecs.bench.iteration")
//
//     BENCH("Each over 100k", 1000) {
//         store.Each<Position, const Velocity>([](Entity, Position &p, const Velocity &v) {
//             p.Value += v.Value;
//         });
//     }
//
// **What a benchmark measures is one iteration.** The body runs `iterations`
// times per sample and the reported figure is nanoseconds per iteration, so two
// benchmarks with different loop counts are comparable and a body that got
// faster shows as a smaller number rather than as a shorter run.
//
// **Warm-up is not optional and not the caller's job.** The first run of
// anything pays for cold caches, lazy page faults and a branch predictor that
// has never seen the code. A harness that included those in the measurement
// would report the allocator, not the algorithm.
//
// **The minimum sample is reported, not the mean.** A benchmark is bounded
// below by the work and unbounded above by whatever else the machine felt like
// doing — a scheduler preemption, a turbo clock stepping down, another suite
// building. The mean of that distribution measures the machine's mood; the
// minimum measures the code. The spread is reported alongside so that a
// suspiciously wide one is visible rather than averaged in.

#include <engine/testing/Suite.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace engine::testing {

	// One declared benchmark.
	//
	// @since v0.2
	struct BenchCase {
		// The suite it belongs to, which is the file's `TEST_SUITE_ID`.
		std::string_view Suite;

		// What it is called. Free text, and the last field on a report line, so
		// tabs and newlines in it are flattened before they are written.
		std::string_view Name;

		// How many times the body runs per sample. Chosen by the author,
		// because only the author knows whether one call is a microsecond or a
		// nanosecond.
		size_t Iterations = 1;

		// Runs the body `Iterations` times.
		std::function<void()> Body;
	};

	// Every benchmark this binary declares.
	//
	// @since v0.2
	class BenchRegistry {
	  public:
		// Declares one. Called at static-initialisation time.
		//
		// @param entry The benchmark.
		static void Declare(BenchCase entry);

		// Everything declared, in declaration order.
		//
		// @return The benchmarks.
		static const std::vector<BenchCase> &All();

	  private:
		BenchRegistry() = delete;
	};

	// Stops the optimiser deleting work whose result nothing reads.
	//
	// A benchmark's body usually computes something and throws it away, which
	// is exactly the shape a compiler is entitled to remove entirely — and a
	// benchmark measuring nothing reports a very good number. Passing the
	// result through here makes the compiler assume something might read it.
	//
	// @param value Anything the body produced.
	template <class T> inline void Consume(const T &value) {
		// An empty asm block that claims to read the value. No instruction is
		// emitted; what it costs is the optimiser's willingness to prove the
		// computation dead.
#if defined(__GNUC__) || defined(__clang__)
		asm volatile("" : : "r,m"(value) : "memory");
#else
		volatile const T sink = value;
		(void)sink;
#endif
	}

	// Registration object. A namespace-scope static, one per macro use.
	struct BenchDeclaration {
		BenchDeclaration(
			std::string_view suite, std::string_view name, size_t iterations, std::function<void()> body
		) {
			BenchRegistry::Declare(BenchCase{suite, name, iterations, std::move(body)});
		}
	};
}

#define ENGINE_BENCH_CONCAT_(a, b) a##b
#define ENGINE_BENCH_CONCAT(a, b) ENGINE_BENCH_CONCAT_(a, b)

// Declares a benchmark whose body runs `iterations` times per sample.
//
// The suite identifier comes from the file's `TEST_SUITE_ID`, the same way
// `TEST_DEPENDS` takes it — repeating it here is the version that drifts.
#define BENCH(name, iterations)                                                                              \
	static void ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)();                                              \
	namespace {                                                                                              \
		const ::engine::testing::BenchDeclaration ENGINE_BENCH_CONCAT(MonoBenchDeclaration, __LINE__){       \
			MonoTestSuiteId, name, (iterations), &ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)               \
		};                                                                                                   \
	}                                                                                                        \
	static void ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)()
