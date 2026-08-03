// What the handover costs, with the work taken out of it.
//
// **Every grain constant in the tree is a ratio, and this is its denominator.**
// `DEFAULT_GRAIN` times `MINIMUM_GRAINS` is an answer to "how much work repays
// waking the pool", so it moves when the work gets cheaper *and* when the wake
// gets dearer — and until this file existed only the first half was measured.
// D00012 found the numerator had halved at `-O3` with nothing in the build
// noticing; the denominator had never been measured on its own at all.
//
// The bodies here do nothing. What is left is the cost the pool imposes on a
// dispatch that gains nothing from it, which is exactly what a span below the
// crossover pays.

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>

TEST_SUITE_ID("engine.parallel.bench.dispatch")

using engine::parallel::Jobs;
using engine::testing::Consume;

namespace dispatch_bench {
	// Started once for the whole binary. Starting a pool inside a measured body
	// would measure the pool's construction rather than its use.
	struct Pool {
		Pool() {
			Jobs::Start(0);
		}
		~Pool() {
			Jobs::Stop();
		}
	};
	const Pool Workers;

	// Nothing, in a form the optimiser cannot delete the call to.
	void Nothing(size_t begin, size_t end) {
		Consume(begin);
		Consume(end);
	}
}

using namespace dispatch_bench;

// --- the control --------------------------------------------------------------

BENCH("For · below the floor, so inline", 10'000) {
	// What a `For` costs when it decides not to dispatch: the worker-count
	// lock, two clock reads and one indirect call. Every figure below is only
	// meaningful as a difference against this one.
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

// --- the handover ---------------------------------------------------------------

BENCH("For · dispatched, 8 empty ranges", 2000) {
	// The marginal case `MINIMUM_GRAINS` describes: exactly the floor, eight
	// grains, and no work in any of them. **This is the number a span has to
	// repay before parallel is worth asking for.**
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 128 empty ranges", 2000) {
	// Sixteen times the ranges and the same nothing in each. If this reads like
	// the row above, the cost is the wake and the join rather than the ranges,
	// and no grain chosen for a cheap body can avoid it.
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(128 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 8 empty ranges, one worker", 20'000) {
	// The same dispatch against a pool of one, which separates the two halves
	// of the cost: what one handover costs, against what it costs to have every
	// other worker wake, find nothing left to take, and then queue on one mutex
	// to say it is done. `Batch::Outstanding` is decremented under
	// `Pool::Guard` by every worker whether it took a range or not, so the join
	// is linear in the pool and not in the work.
	//
	// The pool is restarted inside the body because there is nowhere else to do
	// it — one pool, one process. Twenty thousand iterations is what makes that
	// affordable: about a millisecond of thread churn spread across all of
	// them, which is under two per cent of the figure. It restores the full
	// pool on the way out, and it is last in the file so that nothing measured
	// above is taken against a pool of one.
	Jobs::Stop();
	Jobs::Start(1);
	for (int pass = 0; pass < 20'000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
	Jobs::Stop();
	Jobs::Start(0);
}
