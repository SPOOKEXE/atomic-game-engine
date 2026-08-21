#pragma once

// The heap profiler: where the live bytes are, and whether they are still
// climbing.
//
// This is the third profiler the root `AGENTS.md` names, and it measures the
// one axis the other two cannot. Tracy and `FrameGraph` both answer "where did
// the frame go"; a frame that is fast and 40 MB heavier than the one before it
// reads as healthy on either. Runaway allocation is not a spike, it is a slope,
// and a slope needs a quantity sampled over minutes rather than a duration
// measured over one frame.
//
// **How an allocation finds its tag, and how a free finds it back.** Every
// block carries a header in the bytes immediately before the pointer handed
// out, and that header names the tag tree node the allocation was attributed
// to. So `operator delete` is a pointer subtraction and two atomic decrements:
// no map, no lock, and no lookup in the hottest path a process has. The cost is
// the header itself - see `BlockOverhead` - and that cost is why this is
// compiled out of `release`.
//
// **Every `ENGINE_PROFILE` scope is also a heap tag**, which is where the
// granularity comes from - see `Profiling.hpp`. `ENGINE_HEAP_SCOPE` is for the
// places that allocate and are not worth timing.
//
// **Tags are a tree, not an enum.** A scope pushes a name under whatever scope
// is already open on this thread, so `render` opened inside `frame` is the node
// `frame;render` and not a second unrelated counter. That is what lets live
// bytes fold into the same `root;child;leaf <value>` format `FrameGraph`
// already writes for time, and be drawn by the same flamegraph renderer.
//
// **What it costs, measured rather than reasoned about.** On the machine this
// was written on, `bench` preset: a tracked `new`/`delete` pair of 64 bytes is
// **44 ns against 4 ns** for the same allocator reached through `malloc`, which
// is the header write plus ten relaxed atomics; a tag scope is **5 ns**, and the
// same at depth eight, because the lookup walks one node's child list rather
// than the stack. A client allocating 144,000 blocks a second therefore spends
// about 0.6% of its wall clock being measured. `engine.core.bench.instrumentation`
// carries the rows and re-measures them.
//
// **It is a compile-time switch and not a runtime one.** A block allocated
// while tracking was off has no header, and freeing it while tracking is on
// would read four words of somebody else's memory. So `MONO_HEAP_PROFILE`
// decides once, for the whole program, and there is no way to turn the header
// off in a running process. What *is* runtime is sampling: the history the
// growth report is computed from costs a walk of the tag tree per sample and is
// off until something asks.
//
// @tier L0 · shared
// @since v0.18

#include <engine/core/FrameGraph.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

	// One node of the tag tree, as a reader sees it.
	//
	// The byte figures are *exclusive*: they count blocks allocated while this
	// node was the innermost open scope and not those of its children.
	// `HeapProfile::InclusiveBytes` sums a subtree, because a flamegraph wants
	// the total and a leak hunt wants to know which level it entered at.
	//
	// @since v0.18
	struct HeapNodeView {
		// The scope name, in the profiler's own name arena. Valid for the life
		// of the process, so a panel may hold it across frames.
		std::string_view Name;

		// Index of the enclosing node, or `HeapProfile::ROOT` for a top-level
		// tag. The root is its own parent, which terminates a walk without a
		// sentinel to test for.
		uint32_t Parent = 0;

		// Zero for the root, one for a tag opened with nothing else on the
		// stack. Kept rather than re-derived so a reader does not walk to the
		// root for every row it draws.
		uint32_t Depth = 0;

		// Bytes currently allocated and not yet freed under this node.
		//
		// Signed, and it can go negative. A block allocated under one tag and
		// freed under another is attributed to where it was *allocated* - which
		// is the useful answer - so this is exact. It goes negative only when a
		// block allocated before this process replaced `operator new` is freed
		// through it, which is a real condition and one worth seeing rather than
		// clamping away.
		int64_t LiveBytes = 0;

		// Blocks currently allocated and not yet freed under this node.
		int64_t LiveBlocks = 0;

		// Bytes ever allocated under this node. Never decreases, so
		// `TotalBytes` against `LiveBytes` separates a leak from churn: equal
		// figures are a subsystem that allocated once and kept it, and a total
		// a thousand times the live figure is one recycling every frame.
		uint64_t TotalBytes = 0;

		// Blocks ever allocated under this node.
		uint64_t TotalBlocks = 0;

		// The largest `LiveBytes` ever reached. A subsystem that peaked at
		// 300 MB and settled at 4 MB is a spike a sample every second would
		// miss entirely.
		int64_t PeakBytes = 0;
	};

	// The whole process, summed.
	//
	// @since v0.18
	struct HeapTotals {
		// Bytes allocated and not yet freed, across every node.
		int64_t LiveBytes = 0;

		// Blocks allocated and not yet freed.
		int64_t LiveBlocks = 0;

		// Bytes ever allocated.
		uint64_t TotalBytes = 0;

		// Blocks ever allocated.
		uint64_t TotalBlocks = 0;

		// The largest `LiveBytes` the process ever reached.
		int64_t PeakBytes = 0;

		// Header bytes the profiler itself is costing right now.
		//
		// Reported rather than hidden, because it is not small: it is
		// `BLOCK_OVERHEAD` times `LiveBlocks`, and a subsystem holding a million
		// tiny nodes pays more for being measured than for what it is holding.
		// Somebody comparing a `dev` figure with a `release` one needs this
		// number to do it.
		int64_t OverheadBytes = 0;

		// Frees of a pointer with no recognisable header.
		//
		// Should be zero. It is not an assertion because the one way it happens
		// legitimately - a block allocated by a library that replaced
		// `operator new` before this one, then freed through this one - is a
		// link-order property rather than a bug in the caller, and aborting the
		// process over it would make the profiler less safe than the thing it
		// measures. The block is freed through the system allocator unchanged
		// and counted here.
		uint64_t ForeignFrees = 0;

		// Distinct tag paths in use.
		uint32_t Nodes = 0;

		// Scopes refused because the tag tree was full, or because the scope
		// stack was already `MAXIMUM_DEPTH` deep.
		//
		// Non-zero means the tree is not the whole picture, exactly as
		// `FrameGraph::Dropped` means the flamegraph is not.
		uint64_t DroppedScopes = 0;
	};

	// One reading of the process's live bytes, taken by `HeapProfile::Sample`.
	//
	// @since v0.18
	struct HeapSample {
		// Seconds from the monotonic clock, as `Clock::Seconds` reports it.
		double Seconds = 0.0;

		// Process-wide live bytes at that moment.
		int64_t LiveBytes = 0;

		// Process-wide live blocks at that moment.
		int64_t LiveBlocks = 0;
	};

	// What one tag path did across a sampled window: the runaway report.
	//
	// @since v0.18
	struct HeapGrowth {
		// The full tag path, `root;child;leaf`, of the node this describes.
		std::string Path;

		// Index of the node in the tag tree, for a caller that wants to go back
		// to it.
		uint32_t Node = 0;

		// Least-squares slope of live bytes against time over the window.
		//
		// **A slope rather than last-minus-first, because a heap is noisy.** A
		// subsystem that allocates a 32 MB buffer, frees it, and allocates it
		// again has a last-minus-first of zero on one pair of samples and 32 MB
		// on the next; the slope over sixty of them is near zero either way, and
		// a genuine leak keeps its slope however the samples land.
		double BytesPerSecond = 0.0;

		// How much of the variation in the samples the slope accounts for,
		// between 0 and 1 - the coefficient of determination.
		//
		// **This is what separates a leak from a staircase.** A subsystem that
		// loads a level and holds it has a large slope and a poor fit, because
		// its samples are two flat runs with a step between them. A leak is a
		// line, and a line fits a line. `Fit` is zero when the samples do not
		// vary at all, which is a heap that is not moving rather than one that
		// is unexplained.
		double Fit = 0.0;

		// Live bytes at the first retained sample of the window.
		int64_t FirstBytes = 0;

		// Live bytes at the last.
		int64_t LastBytes = 0;

		// The largest live figure seen anywhere in the window.
		int64_t PeakBytes = 0;
	};

	// One row of the tag tree, flattened for something that draws a list.
	//
	// **Flattened here rather than by each consumer**, because the order is not
	// obvious and getting it wrong is not visible: depth-first with each node's
	// children sorted by what they hold, which is what makes the heaviest path
	// read down the left edge instead of being scattered through the rows in
	// first-use order.
	//
	// @since v0.18
	struct HeapTreeRow {
		// Index in the tag tree.
		uint32_t Node = 0;

		// The scope name, borrowed from the profiler's own name arena, which
		// outlives any panel that draws it.
		std::string_view Name;

		// Nesting level, one for a top-level tag.
		uint32_t Depth = 0;

		// Live bytes of this node and everything beneath it.
		int64_t InclusiveBytes = 0;

		// Live bytes charged to this node alone.
		int64_t SelfBytes = 0;

		// Blocks charged to this node alone.
		int64_t LiveBlocks = 0;
	};

	// The process-wide heap profiler.
	//
	// Reads are thread-safe and so is every scope: a worker opens its own stack
	// and its allocations are attributed to it, which is the difference from
	// `FrameGraph` and the reason for it - a leak on a job thread is still a
	// leak, where a *span* on a job thread is a different thread's stack.
	//
	// @threadsafe
	// @since v0.18
	class HeapProfile {
	  public:
		// Whether the allocator hooks were compiled into this program.
		//
		// False makes every function here answer with zeroes rather than
		// disappear, so a panel and a test both compile against the `release`
		// preset and simply have nothing to show.
		static bool IsCompiledIn();

		// Bytes each tracked block costs on top of what the caller asked for.
		//
		// The header, plus what is spent rounding the user pointer up to its
		// required alignment. Reported so a `dev` footprint can be compared with
		// a `release` one honestly.
		static size_t BlockOverhead();

		// The node every scope stack starts from. Its own parent, and never
		// removed.
		static constexpr uint32_t ROOT = 0;

		// Distinct tag paths the tree will hold.
		//
		// Past this a scope is refused, counted in `HeapTotals::DroppedScopes`,
		// and its allocations land on the enclosing node - so the bytes are
		// still counted and only the attribution is coarser. Bounded because a
		// node is never removed and a caller naming a scope per level would
		// otherwise grow this without limit.
		//
		// **Four thousand, because a node is a *path* and not a name.** Every
		// `ENGINE_PROFILE` site in the engine is also a heap tag, and the same
		// span opened under two different parents is two nodes - so the count
		// is closer to the number of distinct call paths through the
		// instrumented code than to the number of macros in it. A client
		// measured about 300 and a loaded server rather more.
		static constexpr uint32_t MAXIMUM_NODES = 4096;

		// How deep a thread's scope stack may go.
		//
		// The same budget `FrameGraph::MAXIMUM_DEPTH` uses, and the same figure
		// deliberately: the two trees are opened by one macro, so a heap tree
		// that stopped shallower would report a subsystem's allocations against
		// an ancestor whose time the flamegraph beside it reports in full.
		//
		// Past it the depth is still tracked so the matching pop stays balanced,
		// exactly as `FrameGraph` does and for the same reason.
		static constexpr uint32_t MAXIMUM_DEPTH = 32;

		// Opens a tag scope on the calling thread and returns what to restore.
		//
		// **The text is copied, once, the first time this path is seen.** The
		// caller therefore needs it valid only for the duration of the call,
		// which is a weaker promise than `FrameGraph::Scope` asks for and
		// deliberately so: a tag tree node is never removed, so borrowing would
		// need storage that lives as long as the *process*, and the scheduler's
		// system names live only as long as their world.
		//
		// It is still not a place for a name built at runtime. The tree is
		// bounded and never shrinks, so a caller naming a tag per script chunk
		// fills it and everything after that is charged to an ancestor. Name a
		// subsystem, not an instance of one.
		//
		// Prefer `ENGINE_HEAP_SCOPE` over calling this. The pair exists for a
		// caller whose scope is not a C++ block - a resource that is tagged from
		// the moment it is claimed until it is handed on.
		//
		// @param name Scope name, copied on first use. Empty is refused.
		// @return The node that was open before, to be given to `Pop`.
		static uint32_t Push(std::string_view name);

		// Restores the node a matching `Push` returned.
		//
		// @param previous Exactly what the matching `Push` returned.
		static void Pop(uint32_t previous);

		// Returns the node the calling thread is currently allocating into.
		static uint32_t Current();

		// --- reading -----------------------------------------------------------

		// Returns the process totals.
		static HeapTotals Totals();

		// Returns how many nodes the tag tree holds. Node indices below this are
		// valid and stay valid: a node is never removed and the tree never
		// moves.
		static uint32_t NodeCount();

		// Returns one node's counters.
		//
		// @param index A node index below `NodeCount()`. Out of range answers a
		//              default-constructed view rather than reading past the
		//              pool.
		static HeapNodeView Node(uint32_t index);

		// Returns the live bytes of a node and everything beneath it.
		//
		// @param index A node index below `NodeCount()`.
		static int64_t InclusiveBytes(uint32_t index);

		// Returns the tag tree flattened depth-first, heaviest child first.
		//
		// The root is not a row: it has no name, and what it holds is reported
		// as `Totals().LiveBytes` less the sum of the rows.
		//
		// @param minimumBytes A node holding less than this inclusively is left
		//                     out, and so is everything beneath it - a subtree
		//                     that is not worth a row is not worth its children
		//                     either.
		// @return Rows in draw order.
		static std::vector<HeapTreeRow> TreeRows(int64_t minimumBytes = 0);

		// Returns the `root;child;leaf` path of a node, without the root's own
		// name.
		//
		// @param index A node index below `NodeCount()`.
		static std::string Path(uint32_t index);

		// Zeroes every node's totals and peaks and leaves the live figures
		// alone.
		//
		// **Live bytes cannot be reset and must not be**: they describe blocks
		// that still exist, and zeroing them would make the next free take the
		// figure negative. What this clears is the "ever" half - the totals a
		// benchmark wants fresh at the start of a measured section.
		static void ResetTotals();

		// --- sampling ----------------------------------------------------------

		// Readings the history keeps. Past this the oldest is dropped.
		//
		// At one sample a second that is eight and a half minutes, which covers
		// the five-minute soak the runaway benchmark runs with room for a
		// warm-up to be excluded. It is not larger because the per-node series below are
		// `MAXIMUM_SAMPLES` by `MAXIMUM_TRACKED_NODES` by eight bytes, and that
		// product is the whole memory cost of watching.
		static constexpr size_t MAXIMUM_SAMPLES = 512;

		// Nodes whose own series the history keeps.
		//
		// The process total is kept for every sample. A *per node* series is
		// what the growth report needs, and keeping one for every possible node
		// would be `MAXIMUM_NODES` times as much memory - so the first
		// `MAXIMUM_TRACKED_NODES` nodes to exist get one and the rest are
		// covered by the process total alone. Nodes exist in first-use order, so
		// in practice this is every tag the program has, and the limit only
		// bites on a program that names scopes at runtime.
		static constexpr uint32_t MAXIMUM_TRACKED_NODES = 512;

		// Starts or stops recording samples.
		//
		// Turning it *on* clears the history, so a capture covers one run.
		// Turning it off keeps what was recorded, so the growth report can be
		// asked for afterwards.
		//
		// @param enabled Whether `Sample` should record.
		static void SetSamplingEnabled(bool enabled);

		// Reports whether sampling is on.
		static bool IsSampling();

		// Records one reading of every tracked node, if sampling is on.
		//
		// Costs a walk of the tag tree, so it belongs on a timer - once a second
		// is the interval the growth report is tuned for - rather than in a
		// frame loop.
		static void Sample();

		// The interval the growth report is tuned for, and what every program in
		// this repository passes to `SampleIfDue`.
		//
		// A second, because `MAXIMUM_SAMPLES` readings at this rate is a
		// seventeen minute window and a leak worth finding moves more than the
		// noise floor in that time. Faster buys nothing: a slope fitted to a
		// hundred readings a second over ten seconds is fitting the allocator's
		// churn, not the program's.
		static constexpr double SAMPLE_SECONDS = 1.0;

		// Records a reading when `intervalSeconds` have passed since the last
		// one, so a frame loop can call this every frame.
		//
		// **The timer is here rather than in each program**, because otherwise
		// the client, the server and the editor each keep their own last-sample
		// timestamp - three copies of one fact, which rule 2 is about, and three
		// chances for a run to sample at a rate the growth report was not tuned
		// for.
		//
		// @param intervalSeconds Seconds between readings. Zero or less samples
		//                        every call.
		// @return Whether a reading was taken.
		static bool SampleIfDue(double intervalSeconds = SAMPLE_SECONDS);

		// Returns the process-wide readings, oldest first.
		static std::vector<HeapSample> History();

		// Returns the seconds between the oldest and newest retained reading.
		// Fewer than two readings cover zero seconds.
		static double HistorySeconds();

		// Discards every retained reading and keeps sampling on.
		static void ResetHistory();

		// Returns the growth of every tracked node over the retained window,
		// steepest slope first.
		//
		// **Over the newest `window` seconds and not the whole history**, so a
		// caller can exclude a warm-up without having had the foresight to reset
		// at the right moment. A run's first seconds are a level loading, and
		// that is a staircase every time.
		//
		// @param windowSeconds How far back to reach. Zero or more than the
		//                      history covers uses everything retained.
		// @param minimumBytes  Nodes whose live figure never reaches this in the
		//                      window are left out, so the report is not a
		//                      hundred rows of a kilobyte each.
		// @return One entry per surviving node, sorted by `BytesPerSecond`
		//         descending. Empty when fewer than two readings are retained.
		static std::vector<HeapGrowth> Growth(double windowSeconds = 0.0, int64_t minimumBytes = 64 * 1024);

		// How well a slope has to fit before a climb is called a leak.
		//
		// **A threshold in one place, because otherwise every caller picks
		// one.** The number separates a line from a staircase: a subsystem that
		// loaded a level and held it has a large slope and a poor fit, and
		// calling that a leak is how an automatic check earns the reputation
		// that gets it switched off. Measured against the engine's own soak: a
		// scene load reads about 0.5 and a genuine unbounded container reads
		// above 0.95.
		static constexpr double RUNAWAY_FIT = 0.80;

		// Nothing below a megabyte is called a runaway, whatever its slope.
		//
		// A tag that has grown by a few kilobytes over five minutes is a cache
		// filling up, and a check that fails on one is a check nobody runs.
		static constexpr int64_t RUNAWAY_BYTES = 1024 * 1024;

		// Returns the tags climbing faster than `bytesPerSecond` on a slope that
		// actually fits, steepest first.
		//
		// This is the automatic check: an empty result is a heap that reached a
		// steady state, and every entry is a tag path somebody has to explain.
		//
		// @param bytesPerSecond The rate above which a climb is a finding.
		// @param windowSeconds  How far back to fit, as `Growth` takes it. Pass
		//                       the run less its warm-up: a level loading is a
		//                       step, and a step at the start of a window drags
		//                       a slope through everything after it.
		// @param minimumBytes   Floor on how much a tag has to hold.
		static std::vector<HeapGrowth>
		Runaway(double bytesPerSecond, double windowSeconds = 0.0, int64_t minimumBytes = RUNAWAY_BYTES);

		// --- reports -----------------------------------------------------------

		// Adds the live bytes of every node to a folded-stack total.
		//
		// The same interchange format `FrameGraph` folds time into - one line
		// per stack, `root;child;leaf <value>` - so the flamegraph renderer that
		// draws a time profile draws a heap profile with no changes. The value
		// is **bytes**, and it is a caller's job to know which of the two files
		// they opened.
		//
		// Self bytes, exactly as a folded time line carries self time: a
		// renderer sums the prefixes back up, so a node carrying its subtree's
		// total would be counted again by every ancestor.
		//
		// @param totals Added to.
		static void FoldLive(FoldedStacks &totals);

		// Writes the live bytes as folded stacks to `path`.
		//
		// @param path The file to create or replace.
		// @return `false` when nothing is tracked or the file cannot be written.
		static bool WriteFolded(const std::filesystem::path &path);

		// Writes a human-readable report: the totals, the heaviest tags, and the
		// growth over the retained window.
		//
		// This is what a headless run leaves behind, and what the runaway
		// benchmark prints when it fails - a slope with no tag beside it tells
		// somebody there is a leak and nothing about where.
		//
		// @param path           The file to create or replace.
		// @param windowSeconds  Growth window, as `Growth` takes it.
		// @return `false` when the file cannot be opened or writing does not
		//         complete.
		static bool WriteReport(const std::filesystem::path &path, double windowSeconds = 0.0);

		// --- scopes ------------------------------------------------------------

		// Attributes every allocation made on this thread, until the enclosing
		// C++ scope exits, to `name` under whatever scope is already open.
		//
		// Construct through `ENGINE_HEAP_SCOPE` rather than directly.
		class Scope {
		  public:
			// Opens the tag scope.
			//
			// @param name Stable scope name; the text is not copied.
			explicit Scope(std::string_view name) : Previous(Push(name)) {}

			// Restores the enclosing scope.
			~Scope() {
				Pop(Previous);
			}

			// A scope owns one entry on its thread's stack and cannot be copied.
			Scope(const Scope &) = delete;

			// A scope cannot take ownership of another's stack entry by copy.
			Scope &operator=(const Scope &) = delete;

			// A scope cannot move, because destruction order restores the stack.
			Scope(Scope &&) = delete;

			// A scope cannot take ownership of another's stack entry by move.
			Scope &operator=(Scope &&) = delete;

		  private:
			uint32_t Previous;
		};
	};
}

// Joins two preprocessor tokens after the public wrapper expands them.
//
// @param a Left token.
// @param b Right token.
#define ENGINE_HEAP_CONCAT_(a, b) a##b

// Expands and joins two tokens to form each scope helper's unique local name.
//
// @param a Left token.
// @param b Right token.
#define ENGINE_HEAP_CONCAT(a, b) ENGINE_HEAP_CONCAT_(a, b)

#if defined(MONO_HEAP_PROFILE)

// Attributes every allocation on this thread to `name`, until the enclosing
// C++ scope exits.
//
// `name` is copied into the tag tree the first time this path is seen, so it
// need only be valid for the length of the call. Name a subsystem rather than
// an instance of one: the tree is bounded and never shrinks. Two of these may
// share a C++ scope; two on the same *line* may not, which is the same
// restriction the profiling macros carry and for the same reason.
//
// @param name String literal naming the tag.
#define ENGINE_HEAP_SCOPE(name)                                                                              \
	::engine::core::HeapProfile::Scope ENGINE_HEAP_CONCAT(engineHeapScope_, __LINE__) {                      \
		name                                                                                                 \
	}

#else

// Expands to nothing when the allocator hooks are not compiled in.
//
// @param name String literal naming the tag, unused.
#define ENGINE_HEAP_SCOPE(name) ((void)0)

#endif
