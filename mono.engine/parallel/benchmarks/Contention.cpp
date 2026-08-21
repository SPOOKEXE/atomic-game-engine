// What the pool costs when the ranges hold real work.
//
// **`Dispatch.cpp` measures the floor and that is a different question.** An
// empty body reports what a handover costs and says nothing about whether the
// handover paid, because the only thing that decides that is the ratio between
// the work in a range and the cost of giving it away. So every row here runs a
// body with a known, non-removable per-row cost, and the useful reading is a
// division: the serial row against the pooled row is the speedup, and the
// speedup against the worker count is how much of the machine the pool actually
// got.
//
// **Every row does exactly the same million rows of work, so the figures divide
// against each other directly.** The unit is per dispatch rather than per row,
// which looks like the wrong choice until you try the other one: the report
// carries whole nanoseconds, a row here costs single-digit nanoseconds serially,
// and the same row across twenty-four workers costs a fraction of one. A
// per-row column would print the serial figure and then print zero for every
// row the pool actually helped. Same work in every row is what makes the
// coarser unit sound - D00037's complaint is two rows silently in *different*
// units, not a unit that is large.
//
// **What this is looking for.** Three failures, all of which produce a working
// build and a slow frame:
//
// - A grain that dispatches work too small to pay for its own handover. The
//   sweep is what makes the crossover visible instead of a matter of opinion.
// - Ranges that are not the same size. `For` splits by index count, so a body
//   whose cost varies per index leaves the pool waiting on one worker while the
//   rest idle, and the batch costs its slowest range rather than its mean.
// - A dispatch that quietly ran inline. Nested `For` and two threads racing
//   both fall back to the caller by design, and the fallback is *correct* and
//   an order of magnitude slower. A caller that hit it without knowing would
//   see a frame budget vanish with nothing in the profile to blame.
//
// **The pool is process-wide, so a body that changes the worker count puts it
// back.** These share a binary with `Dispatch.cpp`, and a row that left the
// pool at one worker would silently halve every row measured after it.

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.parallel.bench.contention")

using engine::parallel::Jobs;
using engine::testing::Consume;

namespace contention_bench {
	// One dispatch's worth of rows. A million, so that even at the widest
	// worker count each range is large enough that the measurement is the body
	// rather than the handover - the handover is `Dispatch.cpp`'s subject.
	constexpr size_t ROWS = 1'000'000;

	// Small enough that a range of it is cheap and large enough that the pool
	// is worth asking. The grain sweep varies this; every other row takes it.
	constexpr size_t GRAIN = 4096;

	// Enough arithmetic per row that the loop cannot be folded away and the
	// row cost is well above a cache miss, which is what makes a scaling curve
	// mean anything: a body bounded by memory bandwidth scales with the memory
	// system, not with the worker count, and reports that as a pool that does
	// not work.
	//
	// **Thirty-two rather than a handful because the compiler vectorises this.**
	// The steps are a dependent chain per row and the rows are independent, so
	// `-O3` runs eight lanes at once and a smaller count put the whole serial
	// pass under a millisecond - which is inside the noise of the thing being
	// measured.
	constexpr int STEPS = 32;

	// Reused across every row. Allocating four megabytes inside a measured body
	// measures the allocator, and touching it once here means no row pays for
	// the first-touch page faults.
	std::vector<float> &Rows() {
		static std::vector<float> rows = [] {
			std::vector<float> values(ROWS, 1.0f);
			for (size_t index = 0; index < ROWS; index++) {
				values[index] = static_cast<float>(index % 97) + 1.0f;
			}
			return values;
		}();
		return rows;
	}

	// The body every row runs, over the half-open range it was given.
	//
	// Writes only what its own range names, which is the contract `Jobs::For`
	// requires and the reason the same span run whole on one thread produces
	// the same bytes as the same span split twenty ways.
	void Work(std::vector<float> &rows, size_t begin, size_t end) {
		for (size_t index = begin; index < end; index++) {
			float value = rows[index];
			for (int step = 0; step < STEPS; step++) {
				value = value * 1.000001f + 0.5f;
			}
			rows[index] = value;
		}
	}

	// Starts the process-wide pool if nothing has yet.
	//
	// Idempotent, so calling it at the head of a body costs one comparison and
	// removes this file's dependence on another translation unit's static
	// having been constructed first.
	void EnsurePool() {
		Jobs::Start(0);
	}

	// Runs `body` with exactly `workers` pool threads and restores the default.
	//
	// **Restoring is the point.** The pool is process-wide and shared with
	// every other benchmark in this binary, so a row that left it narrowed
	// would be measuring its own change in every row that ran after it.
	template <class Body> void WithWorkers(unsigned workers, Body &&body) {
		Jobs::Stop();
		Jobs::Start(workers);
		body();
		Jobs::Stop();
		Jobs::Start(0);
	}
}

using namespace contention_bench;

// --- the scaling curve --------------------------------------------------------
//
// Read these against each other by division. The serial row is what one core
// does; a pooled row at N workers should approach it divided by N+1, because
// the calling thread drains ranges alongside the pool. A curve that flattens
// early is the body hitting memory bandwidth or the ranges being uneven, and
// the imbalance row below tells the two apart.

BENCH("Serial · 1M rows, no dispatch at all", 1) {
	std::vector<float> &rows = Rows();
	Work(rows, 0, ROWS);
	Consume(rows[0]);
}

BENCH("For · 1M rows · one worker", 1) {
	std::vector<float> &rows = Rows();
	WithWorkers(1, [&rows] {
		Jobs::For(ROWS, GRAIN, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	});
	Consume(rows[0]);
}

BENCH("For · 1M rows · two workers", 1) {
	std::vector<float> &rows = Rows();
	WithWorkers(2, [&rows] {
		Jobs::For(ROWS, GRAIN, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	});
	Consume(rows[0]);
}

BENCH("For · 1M rows · four workers", 1) {
	std::vector<float> &rows = Rows();
	WithWorkers(4, [&rows] {
		Jobs::For(ROWS, GRAIN, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	});
	Consume(rows[0]);
}

BENCH("For · 1M rows · every worker", 1) {
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(ROWS, GRAIN, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	Consume(rows[0]);
}

// --- the grain sweep ----------------------------------------------------------
//
// Same rows, same body, same pool: only the size of a range changes. The
// smallest grain is below what this body is worth dispatching and the largest
// is above what leaves every worker something to take, so the middle of these
// three is where the answer is and the ends are what make it a measurement
// rather than a preference.
//
// `Jobs::DEFAULT_GRAIN` documents at length that 4096 is a compromise between
// two callers that want different numbers. These rows are the evidence for one
// side of that; `engine.ecs.bench.iteration` is the evidence for the other.

BENCH("For · 1M rows · grain 256", 1) {
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(ROWS, 256, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	Consume(rows[0]);
}

BENCH("For · 1M rows · grain 4096", 1) {
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(ROWS, 4096, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	Consume(rows[0]);
}

BENCH("For · 1M rows · grain 65536", 1) {
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(ROWS, 65536, [&rows](size_t begin, size_t end) { Work(rows, begin, end); });
	Consume(rows[0]);
}

// --- ranges that are not the same size ----------------------------------------

BENCH("For · 1M rows · one row in a thousand costs a hundred times more", 1) {
	// **The failure a flat curve hides.** `For` splits by index count because
	// it cannot know what an index costs, so a body whose cost varies leaves
	// the pool finished except for whichever worker drew the expensive ranges.
	// The batch then costs its slowest range and the extra workers bought
	// nothing - which looks exactly like a body that does not parallelise, and
	// is fixed completely differently.
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(ROWS, GRAIN, [&rows](size_t begin, size_t end) {
		for (size_t index = begin; index < end; index++) {
			const int passes = index % 1000 == 0 ? 100 : 1;
			for (int pass = 0; pass < passes; pass++) {
				Work(rows, index, index + 1);
			}
		}
	});
	Consume(rows[0]);
}

// --- the inline fallbacks -----------------------------------------------------

BENCH("For · 1M rows · nested, so the inner dispatch runs inline", 1) {
	// **A nested dispatch is worth exactly what the outer one was worth.** One
	// batch occupies the pool at a time, so the inner `For` does not wait and
	// does not deadlock: it runs its whole span on the thread that asked. What
	// this row measures is the consequence - the sixteen outer blocks are the
	// only division that happened, so the batch gets sixteen-way parallelism on
	// a machine with more workers than that, and finishes when the slowest
	// block does rather than when the last range does.
	//
	// That is the degradation the header promises, priced. It is also how a
	// caller loses the pool with nothing reporting that it did: the answer is
	// right, the wall time is a multiple of what the same work costs flat, and
	// nothing in a profile says the word "inline".
	EnsurePool();
	std::vector<float> &rows = Rows();
	Jobs::For(16, 1, [&rows](size_t outerBegin, size_t outerEnd) {
		for (size_t block = outerBegin; block < outerEnd; block++) {
			const size_t begin = block * (ROWS / 16);
			const size_t end = begin + (ROWS / 16);
			Jobs::For(end - begin, GRAIN, [&rows, begin](size_t innerBegin, size_t innerEnd) {
				Work(rows, begin + innerBegin, begin + innerEnd);
			});
		}
	});
	Consume(rows[0]);
}

// --- a tick's worth of dispatches ---------------------------------------------

BENCH("For · 1M rows · 64 back-to-back dispatches instead of one", 1) {
	// **What a frame actually looks like, and the row that pays for this file.**
	// A tick is not one large batch, it is dozens of small ones from different
	// systems. This is the same million rows as every row above, cut into
	// sixty-four dispatches of about sixteen thousand.
	//
	// It does not dispatch. `Jobs::For` runs a span inline when it is no
	// greater than `DEFAULT_GRAIN * MINIMUM_GRAINS`, and sixteen thousand is
	// under that floor, so every one of the sixty-four calls runs whole on the
	// caller and this row lands on the serial figure. Nothing is wrong and
	// nothing reports anything: a system that batched its work per-chunk
	// instead of per-tick would hand the pool sixty-four spans it silently
	// refuses to split, and the profile would show a busy main thread and idle
	// workers with no line to point at. Read this against `every worker` above,
	// and against `Dispatch.cpp`'s floor rows, which is where the number
	// sixteen thousand comes from.
	EnsurePool();
	std::vector<float> &rows = Rows();
	constexpr size_t BLOCK = ROWS / 64;
	for (size_t dispatch = 0; dispatch < 64; dispatch++) {
		const size_t begin = dispatch * BLOCK;
		Jobs::For(BLOCK, GRAIN, [&rows, begin](size_t innerBegin, size_t innerEnd) {
			Work(rows, begin + innerBegin, begin + innerEnd);
		});
	}
	Consume(rows[0]);
}

// --- placement dispatch -------------------------------------------------------

BENCH("ForWorkers · 1M rows as 64 tasks over the pinned prefix", 1) {
	// The bounded answer when there are more tasks than physical cores, and the
	// one whose cost nothing else measures. Unlike `For` the caller takes no
	// range, so this figure is pure handover plus the serial tail of whichever
	// worker drew the most tasks.
	//
	// With no pinned workers the whole span runs inline, which is a legitimate
	// result on a machine that could not establish core bindings and is why the
	// mapping is built from the count rather than assumed.
	EnsurePool();
	std::vector<float> &rows = Rows();
	const unsigned pinned = Jobs::PinnedWorkerCount();
	static std::vector<unsigned> assignment;
	assignment.resize(64);
	for (size_t task = 0; task < assignment.size(); task++) {
		assignment[task] = pinned == 0 ? 0 : static_cast<unsigned>(task % pinned);
	}

	constexpr size_t BLOCK = ROWS / 64;
	Jobs::ForWorkers(assignment, [&rows](size_t begin, size_t end) {
		for (size_t task = begin; task < end; task++) {
			Work(rows, task * BLOCK, task * BLOCK + BLOCK);
		}
	});
	Consume(rows[0]);
}
