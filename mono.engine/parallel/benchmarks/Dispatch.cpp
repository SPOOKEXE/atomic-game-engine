// Measure dispatch overhead with empty bodies.

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>

TEST_SUITE_ID("engine.parallel.bench.dispatch")

using engine::parallel::JobContext;
using engine::parallel::Jobs;
using engine::testing::Consume;

namespace dispatch_bench {
	// Keep pool construction out of measured bodies.
	struct Pool {
		Pool() {
			Jobs::Start(0);
		}
		~Pool() {
			Jobs::Stop();
		}
	};
	const Pool Workers;

	void Nothing(size_t begin, size_t end) {
		Consume(begin);
		Consume(end);
	}
}

using namespace dispatch_bench;

BENCH("For · below the floor, so inline", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · explicit Serial context", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(JobContext::Serial, 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · explicit Threaded context below the floor", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(JobContext::Threaded, 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 8 empty ranges", 2000) {
	// This is the measured minimum dispatch floor.
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 128 empty ranges", 2000) {
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(128 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 8 empty ranges, one worker", 20'000) {
	// Isolate single-worker handover cost from pool wake-up cost.
	Jobs::Stop();
	Jobs::Start(1);
	for (int pass = 0; pass < 20'000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
	Jobs::Stop();
	Jobs::Start(0);
}

// The two rows below separate "linear in the ranges" from "linear in the
// workers actually woken", which the three rows above cannot: `For` wakes
// `ranges - 1` workers up to the pool size, so every case above varies both at
// once and two points fit either line.

BENCH("For · dispatched, 1024 empty ranges", 2000) {
	// Eight times the ranges of the case above and the same pool, so the woken
	// count is pinned at the pool size and only the range count moves.
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(1024 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 128 empty ranges, four workers", 5000) {
	// The other half: the same ranges as the 128-range case over a pool that
	// can only wake four of them.
	Jobs::Stop();
	Jobs::Start(4);
	for (int pass = 0; pass < 5000; pass++) {
		Jobs::For(128 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
	Jobs::Stop();
	Jobs::Start(0);
}
