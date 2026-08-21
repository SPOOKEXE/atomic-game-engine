// What measuring the engine costs the engine.
//
// Both of the things here are paid by code that is not trying to do anything -
// a counter bumped in a packet handler, a scope opened around a system - so
// their cost is pure overhead and the only defensible number for them is a
// small one.
//
// **The metrics sink is the interesting one, because it is a write-only global
// that every layer above L0 writes to.** `Metrics::Count` takes a name as a
// `string_view` and has to resolve it to a counter on every call; whether that
// resolution is a hash of the text or a cached handle is invisible from the
// header and very visible here. The contended ladder is the shape that matters:
// `net` counts bytes per remote from a socket thread while `script` counts
// allocations from a worker, and if the sink serialises them the seam has
// become a bottleneck rather than a decoupling.
//
// `FrameGraph` is measured both disabled and enabled. Disabled is the one that
// ships - collection is off until something asks for it, so the macros are
// supposed to cost a predictable branch and nothing else, and that claim is
// checkable only by measuring the branch. Enabled is what pressing F5 costs.
//
// **`HeapProfile` is the one row here with no off switch**, and that is why it
// is measured. The other two are silent until somebody asks; the allocator
// hooks are in every `dev` build, on every allocation, whether or not anybody
// ever opens the panel. So the two figures worth having are what a tracked
// `new`/`delete` pair costs against an untracked one, and what a tag scope
// costs on top of the profiling scope it now rides along with.

#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.core.bench.instrumentation")

using engine::core::Clock;
using engine::core::Counter;
using engine::core::FrameGraph;
using engine::core::HeapProfile;
using engine::core::Metrics;
using engine::core::ProfileCategory;
using engine::testing::Consume;

namespace instrumentation_bench {

	// How many distinct counter names the sink is asked to keep apart.
	//
	// A real frame has tens, not thousands: `net.bytes.in`, `ecs.systems.ran`,
	// one per subsystem that thought to report something. A pool of one would
	// measure a single cache line staying hot and say nothing about the lookup.
	constexpr size_t COUNTERS = 64;

	// Counts per row, split across threads on the contended ladder.
	constexpr size_t COUNTS = 50'000;

	// Names of the length and shape the engine actually uses.
	const std::vector<std::string> &CounterNames() {
		static const std::vector<std::string> names = [] {
			std::vector<std::string> built;
			built.reserve(COUNTERS);
			for (size_t index = 0; index < COUNTERS; index++) {
				built.push_back("engine.bench.metric.subsystem" + std::to_string(index));
			}
			return built;
		}();
		return names;
	}

	// Runs `body(worker)` on `threads` threads and waits. The spawn is inside the
	// measurement and is the same on every rung, so read the ladder as ratios.
	template <class Body> void OnThreads(size_t threads, Body body) {
		std::vector<std::thread> workers;
		workers.reserve(threads);
		for (size_t worker = 0; worker < threads; worker++) {
			workers.emplace_back([&body, worker] { body(worker); });
		}
		for (std::thread &worker : workers) {
			worker.join();
		}
	}

	// Turns collection on for the rows that need it and back off afterwards, so
	// the disabled rows are not measured against a graph somebody else enabled.
	// Declaration order inside one file is the harness's execution order, but a
	// row that leaked state would still be a row that made every later suite
	// lie.
	struct Collecting {
		Collecting() {
			FrameGraph::SetEnabled(true);
		}
		~Collecting() {
			FrameGraph::SetEnabled(false);
		}
	};

	// Opens `depth` nested spans and closes them on the way back out.
	//
	// Recursive rather than an array of `Scope`, because a `Scope` uniquely owns
	// its stack entry and is deliberately neither copyable nor movable - the
	// destruction order *is* the close order, and a container would break that.
	// The call itself is a few nanoseconds and lands on both the flat and the
	// nested rows equally, so it does not distort the comparison between them.
	void Nest(size_t depth) {
		const FrameGraph::Scope scope("engine.bench.nested", ProfileCategory::ECS);
		if (depth > 1) {
			Nest(depth - 1);
		}
	}
}

using namespace instrumentation_bench;

// --- the clock ----------------------------------------------------------------

BENCH("Clock::Nanoseconds", 100'000) {
	// **The floor under every other row in this file** and under `ScopedCount`,
	// which is two of these plus an accumulate. On a machine where the clock is
	// a `vDSO` read this is a few nanoseconds; on one where it traps to the
	// kernel it is hundreds, and every profiling decision in the engine changes.
	// Worth knowing which machine you are on before reading anything else.
	uint64_t total = 0;
	for (size_t index = 0; index < 100'000; index++) {
		total += Clock::Nanoseconds();
	}
	Consume(total);
}

// --- the metrics sink ---------------------------------------------------------

BENCH("Metrics::Count · 50k, 64 names", COUNTS) {
	const std::vector<std::string> &names = CounterNames();
	for (size_t index = 0; index < COUNTS; index++) {
		Metrics::Count(names[index % COUNTERS], 1.0);
	}
	Metrics::Clear();
}

BENCH("Metrics::Count · 50k, one name", COUNTS) {
	// The same call count into a single counter. Against the row above, the gap
	// is what the name lookup costs when the answer is not already in L1 - and
	// if there is no gap at all, the sink is resolving the name every call and
	// the 64-name row was flattered by nothing.
	for (size_t index = 0; index < COUNTS; index++) {
		Metrics::Count("engine.bench.metric.single", 1.0);
	}
	Metrics::Clear();
}

BENCH("Metrics::CountTime · 50k", COUNTS) {
	const std::vector<std::string> &names = CounterNames();
	for (size_t index = 0; index < COUNTS; index++) {
		Metrics::CountTime(names[index % COUNTERS], static_cast<uint64_t>(index));
	}
	Metrics::Clear();
}

BENCH("ScopedCount · 50k empty scopes", COUNTS) {
	// What the header calls "cheap enough to leave in a hot path". This row is
	// that sentence as a figure, and it should come out at roughly two
	// `Clock::Nanoseconds` plus one `Metrics::CountTime`. Materially more than
	// that means the destructor is doing something the header does not admit to.
	for (size_t index = 0; index < COUNTS; index++) {
		const engine::core::ScopedCount scope("engine.bench.metric.scope");
		Consume(index);
	}
	Metrics::Clear();
}

BENCH("Metrics::Drain · 64 counters", 1000) {
	// Once per frame, by exactly one reader - that is the property that makes
	// the values a rate rather than a number that only goes up. It allocates a
	// vector every call, so this row is the per-frame price of that allocation
	// and the answer to whether a drain wants a caller-supplied buffer.
	const std::vector<std::string> &names = CounterNames();
	for (size_t pass = 0; pass < 1000; pass++) {
		for (size_t index = 0; index < COUNTERS; index++) {
			Metrics::Count(names[index], 1.0);
		}
		const std::vector<Counter> drained = Metrics::Drain();
		Consume(drained.size());
	}
}

// --- the contended sink -------------------------------------------------------
//
// **Same total work, more threads.** `net` counts from a socket thread while
// `script` counts from a worker and `ecs` counts from every job in the pool, so
// the sink is genuinely written to from everywhere at once - this ladder is
// what says whether that is free. A flat or rising curve means one lock, and
// one lock under the metrics sink is a global variable with extra steps and a
// contention point on top.

BENCH("Metrics::Count contended · 1 thread", COUNTS) {
	const std::vector<std::string> &names = CounterNames();
	OnThreads(1, [&names](size_t worker) {
		for (size_t index = 0; index < COUNTS / 1; index++) {
			Metrics::Count(names[(index + worker * 7) % COUNTERS], 1.0);
		}
	});
	Metrics::Clear();
}

BENCH("Metrics::Count contended · 4 threads", COUNTS) {
	const std::vector<std::string> &names = CounterNames();
	OnThreads(4, [&names](size_t worker) {
		for (size_t index = 0; index < COUNTS / 4; index++) {
			Metrics::Count(names[(index + worker * 7) % COUNTERS], 1.0);
		}
	});
	Metrics::Clear();
}

BENCH("Metrics::Count contended · 8 threads", COUNTS) {
	const std::vector<std::string> &names = CounterNames();
	OnThreads(8, [&names](size_t worker) {
		for (size_t index = 0; index < COUNTS / 8; index++) {
			Metrics::Count(names[(index + worker * 7) % COUNTERS], 1.0);
		}
	});
	Metrics::Clear();
}

BENCH("Metrics::Count contended · 8 threads, one shared name", COUNTS) {
	// The worst case the sink can be given: eight threads onto one counter, so
	// every increment is a write to the same cache line whatever the locking
	// is. Read against the row above - the difference is false sharing rather
	// than lock design, and the fix for the two is not the same fix.
	OnThreads(8, [](size_t) {
		for (size_t index = 0; index < COUNTS / 8; index++) {
			Metrics::Count("engine.bench.metric.shared", 1.0);
		}
	});
	Metrics::Clear();
}

// --- the frame graph, switched off --------------------------------------------
//
// **This is the configuration that ships.** Collection is off unless somebody
// pressed F5, so what these rows measure is the cost of the instrumentation
// being *present* in a build that is not using it. That number belongs in a
// benchmark rather than in an argument, because it is the only thing standing
// between the engine and somebody deciding the macros should be compiled out
// behind an `#ifdef` - which would mean the shipped build and the profiled
// build are no longer the same program.

BENCH("FrameGraph::Scope · disabled, 50k scopes", 50'000) {
	FrameGraph::SetEnabled(false);
	for (size_t index = 0; index < 50'000; index++) {
		const FrameGraph::Scope scope("engine.bench.span", ProfileCategory::Engine);
		Consume(index);
	}
}

BENCH("FrameGraph frame · disabled, 512 scopes", 1000) {
	FrameGraph::SetEnabled(false);
	for (size_t pass = 0; pass < 1000; pass++) {
		FrameGraph::BeginFrame();
		for (size_t index = 0; index < 512; index++) {
			const FrameGraph::Scope scope("engine.bench.span", ProfileCategory::ECS);
			Consume(index);
		}
		FrameGraph::EndFrame();
	}
}

// --- the frame graph, switched on ---------------------------------------------
//
// What pressing F5 costs. The span counts bracket `MAXIMUM_SPANS`, which is
// 4096: a frame under it records everything, and a frame over it is dropping
// spans and drawing a partial flame graph. The pair says whether the overflow
// path is cheaper than the recording one - it must be, or an
// over-instrumented frame gets slower the more it drops, which is the worst
// possible failure mode for a profiler.

BENCH("FrameGraph frame · enabled, 512 flat scopes", 1000) {
	const Collecting collecting;
	for (size_t pass = 0; pass < 1000; pass++) {
		FrameGraph::BeginFrame();
		for (size_t index = 0; index < 512; index++) {
			const FrameGraph::Scope scope("engine.bench.span", ProfileCategory::ECS);
			Consume(index);
		}
		FrameGraph::EndFrame();
	}
}

BENCH("FrameGraph frame · enabled, 512 copied-name scopes", 1000) {
	// `CopiedScope` is the path a script chunk or a node kind takes: the name
	// does not outlive the call, so the text is copied into a pool the frame
	// owns. Against the row above, the difference is that copy - and it is the
	// number that decides whether a subsystem naming its spans at runtime is
	// affordable or has to pre-intern them.
	const Collecting collecting;
	static const std::string runtime = "engine.bench.copied.span";
	for (size_t pass = 0; pass < 1000; pass++) {
		FrameGraph::BeginFrame();
		for (size_t index = 0; index < 512; index++) {
			const FrameGraph::CopiedScope scope("engine.bench.fallback", runtime, ProfileCategory::Script);
			Consume(index);
		}
		FrameGraph::EndFrame();
	}
}

BENCH("FrameGraph frame · enabled, 512 scopes nested 8 deep", 1000) {
	// Depth is what makes a scope tree a tree. `MAXIMUM_DEPTH` is 12 and the
	// first few levels are spent before any real work starts - frame, phase,
	// system - so eight is what a game system's own instrumentation actually
	// sits at. If this row is much dearer than the flat one at the same span
	// count, the parent link is being found by a search rather than held on a
	// stack.
	const Collecting collecting;
	for (size_t pass = 0; pass < 1000; pass++) {
		FrameGraph::BeginFrame();
		for (size_t group = 0; group < 64; group++) {
			Nest(8);
		}
		FrameGraph::EndFrame();
	}
}

BENCH("FrameGraph frame · enabled, 8k scopes over a 4k buffer", 500) {
	// Twice `MAXIMUM_SPANS`, so half of this frame is dropped rather than
	// recorded. The header is explicit that overflow is counted and not
	// resized - reallocating mid-frame would show up in the measurement - so
	// the drop path should be cheaper per span than the record path and this
	// row should come in under twice the 512-scope row scaled up. A row that
	// comes in *over* that means the buffer is still doing work for spans it
	// has already decided to throw away.
	const Collecting collecting;
	for (size_t pass = 0; pass < 500; pass++) {
		FrameGraph::BeginFrame();
		for (size_t index = 0; index < 8192; index++) {
			const FrameGraph::Scope scope("engine.bench.span", ProfileCategory::Render);
			Consume(index);
		}
		FrameGraph::EndFrame();
		Consume(FrameGraph::Dropped());
	}
}

BENCH("FrameGraph::Spans · read back 512", 1000) {
	// What the overlay pays to draw. Reading the published frame has to be
	// cheap and non-copying or the panel costs more than the thing it is
	// panelling.
	const Collecting collecting;
	FrameGraph::BeginFrame();
	for (size_t index = 0; index < 512; index++) {
		const FrameGraph::Scope scope("engine.bench.span", ProfileCategory::Engine);
		Consume(index);
	}
	FrameGraph::EndFrame();

	for (size_t pass = 0; pass < 1000; pass++) {
		Consume(FrameGraph::Spans().size());
		Consume(FrameGraph::FrameMilliseconds());
		Consume(FrameGraph::CategoryMilliseconds(ProfileCategory::Engine));
	}
}

// --- the heap profiler --------------------------------------------------------

BENCH("malloc/free · 200k blocks of 64 bytes", 200'000) {
	// **The untracked floor the row below is read against.** `malloc` does not
	// go through `operator new`, so this is the same allocator doing the same
	// work with no header written and no counter touched - which makes the
	// difference between the two rows the whole price of the heap profiler, on
	// this machine, measured rather than reasoned about.
	// **The pointer is consumed and the memory is written, and both are
	// needed.** Consuming only a `block != nullptr` bool left the allocation
	// provably dead and the optimiser deleted the malloc/free pair outright -
	// the row measured an empty loop and read as three nanoseconds, which is
	// the sort of number that makes everything compared against it look
	// expensive.
	for (size_t index = 0; index < 200'000; index++) {
		auto *block = static_cast<char *>(std::malloc(64));
		Consume(block);
		if (block != nullptr) {
			block[0] = static_cast<char>(index);
			Consume(block[0]);
		}
		std::free(block);
	}
}

BENCH("HeapProfile new/delete · 200k blocks of 64 bytes", 200'000) {
	// **What every allocation in a `dev` build pays.** The hooks are compiled in
	// and cannot be switched off at runtime - a block with no header freed
	// through the tracking `operator delete` would read somebody else's memory -
	// so this is not an opt-in cost like the two profilers above it. Read it
	// against `malloc` on the same machine: the header is 24 bytes and the work
	// is seven relaxed atomics on the way in and three on the way out.
	// Written and consumed exactly as the malloc row above is, so the two
	// differ in the allocator and in nothing else.
	for (size_t index = 0; index < 200'000; index++) {
		auto *block = new char[64];
		Consume(block);
		block[0] = static_cast<char>(index);
		Consume(block[0]);
		delete[] block;
	}
}

BENCH("HeapProfile::Scope · 200k pushes at depth 1", 200'000) {
	// A tag scope on its own. `FindChild` walks the open node's child list
	// comparing string views, and the pointer compare hits first for a literal,
	// so the steady state is a load and a compare. Every `ENGINE_PROFILE` in the
	// engine now pays this, so a figure that is not small here is a figure paid
	// several hundred times a frame.
	for (size_t index = 0; index < 200'000; index++) {
		const HeapProfile::Scope scope("engine.bench.tag");
		Consume(index);
	}
}

BENCH("HeapProfile::Scope · 200k pushes at depth 8", 200'000) {
	// The same push under seven open scopes. It should read the same: the cost
	// is the child-list walk of *one* node, not the depth of the stack. A row
	// that climbs with depth means the lookup is walking to the root.
	const HeapProfile::Scope a("engine.bench.depth1");
	const HeapProfile::Scope b("engine.bench.depth2");
	const HeapProfile::Scope c("engine.bench.depth3");
	const HeapProfile::Scope d("engine.bench.depth4");
	const HeapProfile::Scope e("engine.bench.depth5");
	const HeapProfile::Scope f("engine.bench.depth6");
	const HeapProfile::Scope g("engine.bench.depth7");

	for (size_t index = 0; index < 200'000; index++) {
		const HeapProfile::Scope scope("engine.bench.tag");
		Consume(index);
	}
}

BENCH("HeapProfile::Scope · 200k pushes across 16 siblings", 200'000) {
	// The child list is linear, so a node with many children is a longer walk.
	// Sixteen is more than any real scope has, and the row exists to say what
	// the slope of that would be before somebody adds a hundred.
	static const char *const TAGS[16] = {
		"engine.bench.s00",
		"engine.bench.s01",
		"engine.bench.s02",
		"engine.bench.s03",
		"engine.bench.s04",
		"engine.bench.s05",
		"engine.bench.s06",
		"engine.bench.s07",
		"engine.bench.s08",
		"engine.bench.s09",
		"engine.bench.s10",
		"engine.bench.s11",
		"engine.bench.s12",
		"engine.bench.s13",
		"engine.bench.s14",
		"engine.bench.s15",
	};

	for (size_t index = 0; index < 200'000; index++) {
		const HeapProfile::Scope scope(TAGS[index % 16]);
		Consume(index);
	}
}

BENCH("HeapProfile::Sample · 500 readings of the whole tree", 500) {
	// What watching costs, on the once-a-second clock every program here uses.
	// One walk of the tag tree plus a reverse pass for the inclusive totals, so
	// it scales with the number of tags rather than with allocations - and it
	// is the only part of this profiler that is opt-in.
	HeapProfile::SetSamplingEnabled(true);
	for (size_t pass = 0; pass < 500; pass++) {
		HeapProfile::Sample();
	}
	HeapProfile::SetSamplingEnabled(false);
}
