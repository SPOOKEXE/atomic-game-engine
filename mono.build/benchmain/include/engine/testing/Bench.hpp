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

	// What one iteration of a benchmark is.
	//
	// **The divisor means two different things and a row has to say which.**
	// `BenchMain::Sample` calls a body once and divides the elapsed time by the
	// declared count, so the count is whatever the author is normalising by:
	// `graph/benchmarks/Cull.cpp` loops its body and reports nanoseconds per
	// *call*, while `scene/benchmarks/Ordering.cpp` runs its body once and
	// passes the instance count to report nanoseconds per *instance*. Both are
	// reasonable and the rows are four orders of magnitude apart.
	//
	// Filed as `DEFERRED.md` D00037 after a body that did not loop reported a
	// frame at 739 ns, caught only because another suite's published table gave
	// a number to contradict it.
	//
	// @since v0.11
	enum class BenchUnit : uint8_t {
		// The body ran `Iterations` times; the figure is per call.
		Call,

		// The body ran once over `Iterations` things; the figure is per thing.
		Item,
	};

	// A stable, human-readable name for a unit, as the report writes it.
	//
	// @param unit The unit.
	// @return A view valid for the lifetime of the process.
	constexpr std::string_view Describe(BenchUnit unit) {
		return unit == BenchUnit::Item ? "item" : "call";
	}

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

		// Runs the body `Iterations` times, unless `Unit` says otherwise.
		std::function<void()> Body;

		// What one iteration is, which the report carries so two rows are never
		// silently in different units.
		//
		// @since v0.11
		BenchUnit Unit = BenchUnit::Call;
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
			std::string_view suite,
			std::string_view name,
			size_t iterations,
			std::function<void()> body,
			BenchUnit unit = BenchUnit::Call
		) {
			BenchRegistry::Declare(BenchCase{suite, name, iterations, std::move(body), unit});
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
			MonoTestSuiteId,                                                                                 \
			name,                                                                                            \
			(iterations),                                                                                    \
			&ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__),                                                   \
			::engine::testing::BenchUnit::Call                                                               \
		};                                                                                                   \
	}                                                                                                        \
	static void ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)()

// Declares a benchmark whose body runs **once** over `items` things, reporting
// the cost of one of them.
//
// **The other half of D00037.** `BENCH` promises the body loops; this one
// promises it does not, and the report says which so two rows are never
// silently in different units. Use it when the interesting figure is per
// instance, per byte or per entity rather than per call.
#define BENCH_PER_ITEM(name, items)                                                                          \
	static void ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)();                                              \
	namespace {                                                                                              \
		const ::engine::testing::BenchDeclaration ENGINE_BENCH_CONCAT(MonoBenchDeclaration, __LINE__){       \
			MonoTestSuiteId,                                                                                 \
			name,                                                                                            \
			(items),                                                                                         \
			&ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__),                                                   \
			::engine::testing::BenchUnit::Item                                                               \
		};                                                                                                   \
	}                                                                                                        \
	static void ENGINE_BENCH_CONCAT(MonoBenchBody, __LINE__)()
