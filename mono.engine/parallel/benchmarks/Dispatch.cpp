// Measure dispatch overhead with empty bodies.

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>

TEST_SUITE_ID("engine.parallel.bench.dispatch")

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
