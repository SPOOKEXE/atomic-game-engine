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
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
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

	void SeedSnapshotWorkload() {
		const auto &names = CounterNames();
		for (size_t index = 0; index < COUNTERS; ++index)
			Metrics::Count(names[index], 1.0);
		for (size_t index = 0; index < 8; ++index)
			for (size_t sample = 0; sample < 1024; ++sample)
				Metrics::Observe(names[index], static_cast<double>(sample));
	}

	void VerifySnapshot(const engine::core::MetricsSnapshot &taken) {
		static const auto ordered = [] {
			auto names = CounterNames();
			std::sort(names.begin(), names.end());
			return names;
		}();
		if (taken.Counters.size() != COUNTERS || !taken.Gauges.empty() || taken.Histograms.size() != 8)
			throw std::runtime_error("Metrics Snapshot workload shape differs from oracle");
		for (size_t index = 0; index < COUNTERS; ++index) {
			const auto &counter = taken.Counters[index];
			if (counter.Name.Text() != ordered[index] || counter.Value != 1.0 || counter.Samples != 1 ||
				counter.IsTime)
				throw std::runtime_error("Metrics Snapshot counter fields or ordering differ from oracle");
		}
		for (size_t index = 0; index < 8; ++index) {
			const auto &shape = taken.Histograms[index];
			if (shape.Name.Text() != CounterNames()[index] || shape.Samples != 1024 ||
				shape.Sum != 523776.0 || shape.Minimum != 0.0 || shape.Maximum != 1023.0 ||
				shape.Mean != 511.5 || shape.Retained != 1024 || shape.P50 != 512.0 || shape.P95 != 972.0 ||
				shape.P99 != 1013.0 || shape.IsTime)
				throw std::runtime_error("Metrics Snapshot histogram fields differ from oracle");
		}
	}

	// One preflight, before the first warmup body, leaves the original measured
	// setup and 100 Snapshot calls intact. Both snapshots must reset nothing.
	void SnapshotPreflight() {
		static const bool verified = [] {
			Metrics::Clear();
			SeedSnapshotWorkload();
			const auto first = Metrics::Snapshot();
			VerifySnapshot(first);
			VerifySnapshot(Metrics::Snapshot());
			Metrics::Clear();
			VerifySnapshot(first);
			return true;
		}();
		Consume(verified);
	}

	constexpr std::string_view SNAPSHOT_SCOPE = "Metrics::Snapshot diagnostic";

	engine::core::HeapNodeView SnapshotHeap() {
		const uint32_t count = HeapProfile::NodeCount();
		if (count > 4096) throw std::runtime_error("Metrics Snapshot heap tree exceeds diagnostic bound");
		engine::core::HeapNodeView result;
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Name != SNAPSHOT_SCOPE) continue;
			result.TotalBytes += node.TotalBytes;
			result.TotalBlocks += node.TotalBlocks;
			result.LiveBytes += node.LiveBytes;
			result.LiveBlocks += node.LiveBlocks;
			result.PeakBytes += node.PeakBytes;
		}
		return result;
	}

	struct RestoreSnapshotProfile {
		bool Enabled = FrameGraph::IsEnabled();
		~RestoreSnapshotProfile() {
			FrameGraph::SetEnabled(Enabled);
		}
	};

	// The root heap tag contains each returned snapshot and its destruction.
	// Capture/counter reads and reporting are opt-in broader BENCH overhead.
	void SnapshotMany() {
		static const bool diagnostic = [] {
			const char *value = std::getenv("ATOMIC_METRICS_SNAPSHOT_PROFILE");
			return value != nullptr && std::string_view(value) == "1";
		}();
		if (!diagnostic) {
			for (size_t pass = 0; pass < 100; ++pass) {
				const auto taken = Metrics::Snapshot();
				Consume(taken.Counters.size() + taken.Histograms.size());
			}
			return;
		}
		static size_t calls = 0;
		if (!HeapProfile::IsCompiledIn() || calls == 128)
			throw std::runtime_error("Metrics Snapshot profile requires heap hooks and at most 128 calls");
		const RestoreSnapshotProfile restore;
		FrameGraph::SetEnabled(true);
		const auto before = SnapshotHeap();
		const auto totalsBefore = HeapProfile::Totals();
		float inclusive = 0, self = 0, frame = 0, unmarked = 0;
		size_t spanCount = 0;
		for (size_t pass = 0; pass < 100; ++pass) {
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("Metrics::Snapshot diagnostic");
				const auto taken = Metrics::Snapshot();
				Consume(taken.Counters.size() + taken.Histograms.size());
			} catch (...) {
				// Close the owner frame on refusal before restoring collector state.
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			const auto &spans = FrameGraph::Spans();
			if (FrameGraph::Dropped() != 0 || spans.size() != 1 || spans.front().Name != SNAPSHOT_SCOPE ||
				spans.front().Depth != 0 || spans.front().Parent != FrameGraph::NO_PARENT)
				throw std::runtime_error("Metrics Snapshot profile incomplete hierarchy or dropped spans");
			spanCount += spans.size();
			inclusive += spans.front().Milliseconds;
			self += spans.front().SelfMilliseconds;
			frame += FrameGraph::FrameMilliseconds();
			unmarked += FrameGraph::UnmarkedMilliseconds();
		}
		const auto after = SnapshotHeap();
		const auto totalsAfter = HeapProfile::Totals();
		if (totalsAfter.DroppedScopes != totalsBefore.DroppedScopes)
			throw std::runtime_error("Metrics Snapshot profile dropped heap scopes");
		++calls;
		std::printf(
			"# metrics-snapshot-profile call=%zu warmup=%d snapshots=100 frames=100 spans=%zu "
			"frame_drops=0 heap_drop_delta=0 inclusive_ms=%.6f self_ms=%.6f frame_ms=%.6f "
			"unmarked_ms=%.6f heap_coverage=cxx_new_delete allocated_bytes=%" PRIu64 " "
			"allocated_blocks=%" PRIu64 " live_bytes_before=%" PRId64 " live_bytes_after=%" PRId64 " "
			"live_blocks_before=%" PRId64 " live_blocks_after=%" PRId64 " "
			"sum_tag_peak_bytes=%" PRId64 " profiler_overhead_bytes=%" PRId64 "\n",
			calls,
			calls <= 8,
			spanCount,
			inclusive,
			self,
			frame,
			unmarked,
			after.TotalBytes - before.TotalBytes,
			after.TotalBlocks - before.TotalBlocks,
			before.LiveBytes,
			after.LiveBytes,
			before.LiveBlocks,
			after.LiveBlocks,
			after.PeakBytes,
			totalsAfter.OverheadBytes
		);
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

BENCH("Metrics::SetGauge · 50k, 64 names", COUNTS) {
	// A gauge replaces where a counter accumulates, so this row should sit on
	// top of `Metrics::Count`: the same lock and the same name lookup, one
	// store instead of one add. A gap means the gauge table is being searched
	// differently from the counter table for no reason.
	const std::vector<std::string> &names = CounterNames();
	for (size_t index = 0; index < COUNTS; index++) {
		Metrics::SetGauge(names[index % COUNTERS], static_cast<double>(index));
	}
	Metrics::Clear();
}

BENCH("Metrics::Observe · 50k, 64 names", COUNTS) {
	// **The row that says whether a histogram may be put on a hot path.** It is
	// the counter's lock and lookup plus a bounded-ring store, and the ring is
	// allocated with the entry rather than grown - so if this is materially
	// above `Metrics::Count`, something is allocating per observation.
	const std::vector<std::string> &names = CounterNames();
	for (size_t index = 0; index < COUNTS; index++) {
		Metrics::Observe(names[index % COUNTERS], static_cast<double>(index % 97));
	}
	Metrics::Clear();
}

BENCH("Metrics::Snapshot · 64 counters and 8 histograms", 100) {
	// **Not a per-frame call, and this row is why.** A snapshot sorts every
	// kind by name and takes three percentiles per histogram over a window of
	// up to 1024 readings, which is a report's price rather than a frame's. The
	// server takes one at shutdown and, when asked, on an interval in seconds.
	SnapshotPreflight();
	SeedSnapshotWorkload();
	SnapshotMany();
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

namespace metrics_lookup_bench {
	using engine::core::MetricsSnapshot;
	constexpr size_t CALLS = 8192;
	constexpr std::string_view OWNER = "Metrics registered lookup diagnostic";
	enum class Operation {
		Count,
		CountTime,
		Gauge,
		Observe,
		ObserveTime,
		Get,
		GetGauge,
		GetHistogram,
		Register,
		Refill,
		Mixed
	};
	constexpr std::string_view OPERATIONS[] = {
		"count",
		"count-time",
		"gauge",
		"observe",
		"observe-time",
		"get",
		"get-gauge",
		"get-histogram",
		"register",
		"refill",
		"mixed"
	};
	struct Row {
		std::string Text;
		double Total = 0, Level = 0;
		uint64_t Counts = 0, Writes = 0;
		bool CounterPresent = false, GaugePresent = false, HistogramPresent = false;
		bool CounterTime = false, HistogramTime = false;
		std::vector<double> Observations;
	};
	void Require(bool value) {
		if (!value) throw std::runtime_error("Metrics lookup full semantic oracle mismatch");
	}
	void Model(Row &row, Operation operation, size_t serial) {
		const double value = static_cast<double>(serial % 127 + 1);
		if (operation == Operation::Count || operation == Operation::CountTime ||
			operation == Operation::Register || operation == Operation::Refill ||
			operation == Operation::Mixed) {
			if (!row.CounterPresent)
				row.CounterTime =
					operation == Operation::CountTime || (operation == Operation::Mixed && serial % 3 == 1);
			row.CounterPresent = true;
			row.Total += value;
			++row.Counts;
		}
		if (operation == Operation::Gauge || operation == Operation::Mixed) {
			row.GaugePresent = true;
			row.Level = value;
			++row.Writes;
		}
		if (operation == Operation::Observe || operation == Operation::ObserveTime ||
			operation == Operation::Mixed) {
			if (!row.HistogramPresent)
				row.HistogramTime =
					operation == Operation::ObserveTime || (operation == Operation::Mixed && serial % 3 == 1);
			row.HistogramPresent = true;
			row.Observations.push_back(value);
		}
	}
	void VerifyCounter(const engine::core::Counter &actual, const Row &expected) {
		Require(
			actual.Name.Text() == expected.Text && actual.Value == expected.Total &&
			actual.Samples == expected.Counts && actual.IsTime == expected.CounterTime
		);
	}
	void VerifyGauge(const engine::core::Gauge &actual, const Row &expected) {
		Require(
			actual.Name.Text() == expected.Text && actual.Value == expected.Level &&
			actual.Writes == expected.Writes
		);
	}
	void VerifyHistogram(const engine::core::Histogram &actual, const Row &expected) {
		const auto &values = expected.Observations;
		Require(!values.empty());
		double sum = 0;
		for (double value : values)
			sum += value;
		const size_t retained = std::min<size_t>(values.size(), Metrics::RETAINED_OBSERVATIONS);
		std::vector<double> tail(values.end() - retained, values.end());
		std::sort(tail.begin(), tail.end());
		const auto percentile = [&](double fraction) {
			return tail[static_cast<size_t>(fraction * static_cast<double>(retained - 1) + 0.5)];
		};
		Require(
			actual.Name.Text() == expected.Text && actual.Samples == values.size() && actual.Sum == sum &&
			actual.Minimum == *std::min_element(values.begin(), values.end()) &&
			actual.Maximum == *std::max_element(values.begin(), values.end()) &&
			actual.Mean == sum / values.size() && actual.Retained == retained &&
			actual.P50 == percentile(.50) && actual.P95 == percentile(.95) && actual.P99 == percentile(.99) &&
			actual.IsTime == expected.HistogramTime
		);
	}
	void Verify(const MetricsSnapshot &actual, const std::vector<Row> &rows) {
		size_t counters = 0, gauges = 0, histograms = 0;
		std::vector<const Row *> sorted;
		for (const auto &row : rows)
			sorted.push_back(&row);
		std::sort(sorted.begin(), sorted.end(), [](auto *a, auto *b) { return a->Text < b->Text; });
		for (const auto *row : sorted) {
			if (row->CounterPresent) {
				Require(counters < actual.Counters.size());
				VerifyCounter(actual.Counters[counters++], *row);
			}
			if (row->GaugePresent) {
				Require(gauges < actual.Gauges.size());
				VerifyGauge(actual.Gauges[gauges++], *row);
			}
			if (row->HistogramPresent) {
				Require(histograms < actual.Histograms.size());
				VerifyHistogram(actual.Histograms[histograms++], *row);
			}
		}
		Require(
			counters == actual.Counters.size() && gauges == actual.Gauges.size() &&
			histograms == actual.Histograms.size()
		);
	}
	void Write(const Row &row, Operation operation, size_t serial) {
		const double value = static_cast<double>(serial % 127 + 1);
		switch (operation) {
		case Operation::Count:
		case Operation::Register:
		case Operation::Refill:
			Metrics::Count(row.Text, value);
			break;
		case Operation::CountTime:
			Metrics::CountTime(row.Text, static_cast<uint64_t>(value));
			break;
		case Operation::Gauge:
			Metrics::SetGauge(row.Text, value);
			break;
		case Operation::Observe:
			Metrics::Observe(row.Text, value);
			break;
		case Operation::ObserveTime:
			Metrics::ObserveTime(row.Text, static_cast<uint64_t>(value));
			break;
		case Operation::Get: {
			const auto actual = Metrics::Get(row.Text);
			Require(actual.has_value());
			VerifyCounter(*actual, row);
			break;
		}
		case Operation::GetGauge: {
			const auto actual = Metrics::GetGauge(row.Text);
			Require(actual.has_value());
			VerifyGauge(*actual, row);
			break;
		}
		case Operation::GetHistogram: {
			const auto actual = Metrics::GetHistogram(row.Text);
			Require(actual.has_value());
			VerifyHistogram(*actual, row);
			break;
		}
		case Operation::Mixed:
			if (serial % 3 == 1)
				Metrics::CountTime(row.Text, static_cast<uint64_t>(value));
			else
				Metrics::Count(row.Text, value);
			Metrics::SetGauge(row.Text, value);
			if (serial % 3 == 1)
				Metrics::ObserveTime(row.Text, static_cast<uint64_t>(value));
			else
				Metrics::Observe(row.Text, value);
			break;
		}
	}
	engine::core::HeapNodeView OwnerHeap() {
		engine::core::HeapNodeView total;
		Require(HeapProfile::NodeCount() <= 4096);
		for (uint32_t id = 1; id < HeapProfile::NodeCount(); ++id) {
			const auto row = HeapProfile::Node(id);
			if (row.Name != OWNER) continue;
			total.TotalBytes += row.TotalBytes;
			total.TotalBlocks += row.TotalBlocks;
			total.LiveBytes += row.LiveBytes;
			total.LiveBlocks += row.LiveBlocks;
			total.PeakBytes += row.PeakBytes;
		}
		return total;
	}
	void Fixture(size_t cardinality, Operation operation, size_t threads, size_t phase) {
		Metrics::Clear();
		std::vector<Row> rows(cardinality);
		// Intern in ascending order, register in descending order to separate id and row order.
		for (size_t index = 0; index < cardinality; ++index) {
			rows[index].Text = "engine.bench.lookup." + std::to_string(index);
			Consume(engine::core::Name(rows[index].Text));
		}
		const bool cold = operation == Operation::Register;
		if (!cold)
			for (size_t index = cardinality; index-- > 0;) {
				const auto seed = operation == Operation::Get			 ? Operation::Count
								  : operation == Operation::GetGauge	 ? Operation::Gauge
								  : operation == Operation::GetHistogram ? Operation::Observe
																		 : operation;
				Write(rows[index], seed, index);
				Model(rows[index], seed, index);
			}
		if (operation == Operation::Refill) {
			const auto drained = Metrics::Drain();
			Require(drained.size() == cardinality);
			for (size_t index = 0; index < cardinality; ++index)
				VerifyCounter(drained[index], rows[cardinality - 1 - index]);
			for (auto &row : rows) {
				row.CounterPresent = false;
				row.Total = 0;
				row.Counts = 0;
			}
		}
		const auto expectedReads = rows;
		const size_t calls = cold ? cardinality : CALLS;
		// Oracle work and storage are outside the profiled operation boundary.
		if (operation != Operation::Get && operation != Operation::GetGauge &&
			operation != Operation::GetHistogram)
			for (size_t serial = 0; serial < calls; ++serial)
				Model(rows[serial % cardinality], operation, serial);
		std::vector<std::optional<engine::core::Counter>> readCounters;
		std::vector<std::optional<engine::core::Gauge>> readGauges;
		std::vector<std::optional<engine::core::Histogram>> readHistograms;
		if (operation == Operation::Get) readCounters.resize(calls);
		if (operation == Operation::GetGauge) readGauges.resize(calls);
		if (operation == Operation::GetHistogram) readHistograms.resize(calls);
		const auto perform = [&](size_t serial) {
			const auto &row = expectedReads[serial % cardinality];
			if (operation == Operation::Get)
				readCounters[serial] = Metrics::Get(row.Text);
			else if (operation == Operation::GetGauge)
				readGauges[serial] = Metrics::GetGauge(row.Text);
			else if (operation == Operation::GetHistogram)
				readHistograms[serial] = Metrics::GetHistogram(row.Text);
			else
				Write(row, operation, serial);
		};
		const bool enabled = FrameGraph::IsEnabled();
		FrameGraph::SetEnabled(true);
		const auto heapBefore = OwnerHeap();
		const auto totalsBefore = HeapProfile::Totals();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("Metrics registered lookup diagnostic");
			if (threads == 1)
				for (size_t serial = 0; serial < calls; ++serial)
					perform(serial);
			else {
				std::vector<std::thread> workers;
				workers.reserve(threads);
				try {
					for (size_t worker = 0; worker < threads; ++worker)
						workers.emplace_back([&, worker] {
							for (size_t serial = worker; serial < calls; serial += threads)
								perform(serial);
						});
				} catch (...) {
					for (auto &worker : workers)
						worker.join();
					throw;
				}
				{
					ENGINE_PROFILE_CAT("Metrics workers join", ProfileCategory::Idle);
					for (auto &worker : workers)
						worker.join();
				}
			}
		} catch (...) {
			FrameGraph::EndFrame();
			FrameGraph::SetEnabled(enabled);
			throw;
		}
		FrameGraph::EndFrame();
		const auto published = FrameGraph::Spans();
		Require(published.size() <= 2);
		std::array<engine::core::FrameSpan, 2> retained;
		std::copy(published.begin(), published.end(), retained.begin());
		const std::span<const engine::core::FrameSpan> spans(retained.data(), published.size());
		Require(spans.size() == (threads == 1 ? 1 : 2) && FrameGraph::Dropped() == 0);
		if (threads > 1)
			Require(
				spans[1].Name == "Metrics workers join" && spans[1].Parent == 0 && spans[1].Depth == 1 &&
				!spans[1].Reported && spans[1].Category == ProfileCategory::Idle
			);
		const auto span = spans.front();
		Require(
			span.Name == OWNER && span.Depth == 0 && span.Parent == FrameGraph::NO_PARENT && !span.Reported &&
			span.Category == ProfileCategory::Engine
		);
		const double frame = FrameGraph::FrameMilliseconds(), unmarked = FrameGraph::UnmarkedMilliseconds();
		const auto heapAfter = OwnerHeap();
		const auto totalsAfter = HeapProfile::Totals();
		FrameGraph::SetEnabled(enabled);
		Require(totalsBefore.DroppedScopes == totalsAfter.DroppedScopes);
		for (size_t serial = 0; serial < calls; ++serial) {
			const auto &row = expectedReads[serial % cardinality];
			if (operation == Operation::Get) {
				Require(readCounters[serial].has_value());
				VerifyCounter(*readCounters[serial], row);
			}
			if (operation == Operation::GetGauge) {
				Require(readGauges[serial].has_value());
				VerifyGauge(*readGauges[serial], row);
			}
			if (operation == Operation::GetHistogram) {
				Require(readHistograms[serial].has_value());
				VerifyHistogram(*readHistograms[serial], row);
			}
		}
		Verify(Metrics::Snapshot(), rows);
		Verify(Metrics::Snapshot(), rows);
		Require(
			!Metrics::Get("engine.bench.lookup.missing") &&
			!Metrics::GetGauge("engine.bench.lookup.missing") &&
			!Metrics::GetHistogram("engine.bench.lookup.missing")
		);
		Verify(Metrics::Snapshot(), rows);
		const auto drained = Metrics::Drain();
		const bool counter = !rows.empty() && rows.front().CounterPresent;
		Require(drained.size() == (counter ? cardinality : 0));
		for (size_t index = 0; index < drained.size(); ++index)
			VerifyCounter(
				drained[index],
				rows[(cold || operation == Operation::Refill) ? index : cardinality - 1 - index]
			);
		for (auto &row : rows) {
			row.CounterPresent = false;
			row.Total = 0;
			row.Counts = 0;
		}
		Verify(Metrics::Snapshot(), rows);
		Metrics::Clear();
		Verify(Metrics::Snapshot(), {});
		const auto totalsCleared = HeapProfile::Totals();
		std::printf(
			"# metrics-lookup phase=%zu warmup=%d cardinality=%zu operation=%.*s threads=%zu calls=%zu "
			"metric_operations=%zu oracle=full-fields owner_scope=1 owner_thread_join_inclusive=%d "
			"inclusive_ms=%.9f self_ms=%.9f frame_ms=%.9f unmarked_ms=%.9f heap_available=1 "
			"owner_allocated_bytes=%" PRIu64 " owner_allocated_blocks=%" PRIu64 " owner_live_before=%" PRId64
			" owner_live_after=%" PRId64 " owner_blocks_before=%" PRId64 " owner_blocks_after=%" PRId64
			" owner_peak_bytes=%" PRId64 " profiler_overhead_bytes=%" PRId64 "\n",
			phase,
			phase <= 8,
			cardinality,
			static_cast<int>(OPERATIONS[static_cast<size_t>(operation)].size()),
			OPERATIONS[static_cast<size_t>(operation)].data(),
			threads,
			calls,
			calls * (operation == Operation::Mixed ? 3 : 1),
			threads > 1,
			span.Milliseconds,
			span.SelfMilliseconds,
			frame,
			unmarked,
			heapAfter.TotalBytes - heapBefore.TotalBytes,
			heapAfter.TotalBlocks - heapBefore.TotalBlocks,
			heapBefore.LiveBytes,
			heapAfter.LiveBytes,
			heapBefore.LiveBlocks,
			heapAfter.LiveBlocks,
			heapAfter.PeakBytes,
			totalsAfter.OverheadBytes
		);
		std::printf(
			"# metrics-lookup-detail phase=%zu cardinality=%zu operation=%.*s threads=%zu start_ms=%.9f "
			"idle_ms=%.9f parent=%u depth=%u reported=%d frame_drops=0 heap_drops=0 "
			"process_allocated_bytes=%" PRIu64 " process_allocated_blocks=%" PRIu64
			" process_live_before=%" PRId64 " process_live_after=%" PRId64
			" process_live_after_clear=%" PRId64 " process_blocks_before=%" PRId64
			" process_blocks_after=%" PRId64 " process_blocks_after_clear=%" PRId64
			" process_peak_bytes=%" PRId64 " profiler_overhead_before=%" PRId64
			" retained_index_capacity_registration=%d name_registry_warm=1 "
			"time_values=fixed_nanoseconds_no_clock "
			"worker_heap_attribution=process_only\n",
			phase,
			cardinality,
			static_cast<int>(OPERATIONS[static_cast<size_t>(operation)].size()),
			OPERATIONS[static_cast<size_t>(operation)].data(),
			threads,
			span.StartMilliseconds,
			span.IdleMilliseconds,
			span.Parent,
			span.Depth,
			span.Reported,
			totalsAfter.TotalBytes - totalsBefore.TotalBytes,
			totalsAfter.TotalBlocks - totalsBefore.TotalBlocks,
			totalsBefore.LiveBytes,
			totalsAfter.LiveBytes,
			totalsCleared.LiveBytes,
			totalsBefore.LiveBlocks,
			totalsAfter.LiveBlocks,
			totalsCleared.LiveBlocks,
			totalsAfter.PeakBytes,
			totalsBefore.OverheadBytes,
			cold
		);
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &entry = spans[index];
			std::printf(
				"# metrics-lookup-span phase=%zu cardinality=%zu operation=%.*s threads=%zu index=%zu "
				"name=%s category=%s parent=%u depth=%u start_ms=%.9f inclusive_ms=%.9f self_ms=%.9f "
				"idle_ms=%.9f "
				"reported=%d\n",
				phase,
				cardinality,
				static_cast<int>(OPERATIONS[static_cast<size_t>(operation)].size()),
				OPERATIONS[static_cast<size_t>(operation)].data(),
				threads,
				index,
				index == 0 ? "metrics.lookup.owner" : "metrics.lookup.join",
				index == 0 ? "engine" : "IDLE",
				entry.Parent,
				entry.Depth,
				entry.StartMilliseconds,
				entry.Milliseconds,
				entry.SelfMilliseconds,
				entry.IdleMilliseconds,
				entry.Reported
			);
		}
	}
	void Run() {
		const char *enabled = std::getenv("ATOMIC_METRICS_LOOKUP_PROFILE");
		if (!enabled || std::string_view(enabled) != "1") return;
		Require(HeapProfile::IsCompiledIn());
		static size_t phase = 0;
		Require(++phase <= 13);
		for (size_t cardinality : {1u, 8u, 64u, 256u}) {
			for (size_t operation = 0; operation < 11; ++operation)
				Fixture(cardinality, static_cast<Operation>(operation), 1, phase);
			for (size_t threads : {4u, 8u}) {
				Fixture(cardinality, Operation::Count, threads, phase);
				Fixture(cardinality, Operation::CountTime, threads, phase);
			}
		}
	}
}
BENCH("Metrics registered lookup diagnostic", 1) {
	metrics_lookup_bench::Run();
}
