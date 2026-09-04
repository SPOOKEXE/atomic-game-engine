#pragma once

// A fixed hierarchy of uniform hash grids over boxes, rebuilt from scratch.
//
// **The grid is rebuilt, never edited.** There is no `Insert`, no `Move` and no
// `Remove`, and adding one would not be an extension - it would be a different
// data structure. This one is count-then-fill: one pass to measure, a prefix
// sum, one pass to place every proxy into one flat array at its exclusive level,
// with no per-cell vector and no allocation once the arrays have reached their
// size. An editable grid needs a per-cell list with holes in it, which allocates, fragments, and
// iterates in an order that depends on the history of the edits rather than on
// the contents. Both designs were weighed when this was chosen; the
// rebuild-only grid is the one written as a standing rule, so this is that one.
//
// **The cost of that is real and is not hidden here:** everything in the index
// is re-measured on every `Rebuild`, including the static geometry that has not
// moved since the world loaded. The answer to that is a second grid holding the
// static proxies and rebuilt when the scene changes, not a mutable grid. Two
// grids and two queries is a caller's problem to solve; a half-editable grid
// would be everyone's.
//
// **Nothing here knows what an entity is.** An id is whatever 64-bit number the
// caller put in. See `AGENTS.md` in this module: an edge to `ecs` would pass
// both the tier check and the layer rule, so that file is the only thing that
// catches it.
//
// @tier L6 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace engine::spatial {

	// One thing the index knows about.
	//
	// The layers are **in the proxy** rather than looked up per candidate. A
	// grid that held only `{id, box}` would have to resolve every candidate
	// through whatever owns the layers to filter it - one random access per
	// candidate, which is precisely the cost an index exists to remove - and it
	// would make this module depend on the ECS to do it.
	//
	// @since v0.4
	struct Proxy {
		// What the caller calls this. Copied through to a hit unchanged.
		uint64_t Id = 0;

		// Its world-space box. The index tests against this and nothing finer;
		// a caller wanting the exact shape tests the candidates itself.
		core::AABB Bounds;

		// Which layers it is on. Empty by default, matching nothing - see
		// `LayerMask`, which explains why that is the safer of the two
		// wrong-by-default answers.
		LayerMask Layers;
	};

	// Logical storage owned by one grid. Live bytes describe the current index;
	// retained bytes describe its reusable high-water capacity.
	// @since v0.22
	struct HashGridStats {
		size_t LiveBytes = 0;
		size_t RetainedBytes = 0;
	};

	// Fixed uniform grids of cells, each hashed into its own fixed bucket table.
	//
	// Build one per set of proxies that move together, then query it. It owns the
	// proxies because every candidate a bucket produces is re-tested against its
	// own box and the test needs the box. A proxy belongs to the finest level
	// where it fits the entry bound. Coordinate clamping keeps non-finite bounds
	// in bounded cells. Only inverted, hierarchy-exhausting, or density-rejected
	// ranges use the exact residual list instead. `Rebuild` copies a ready span;
	// `RebuildGenerated` lets a gather write straight into that owned storage.
	//
	// @since v0.4
	class HashGrid {
	  public:
		// A caller-owned blocking range dispatcher.
		//
		// Spatial deliberately knows nothing about the job system. The caller may
		// run this inline, hand it to `parallel::Jobs`, or use a test dispatcher
		// that permutes ranges. `Run` must not return until every body range has
		// completed. The pointers are non-owning and valid only for that call,
		// which keeps the adapter smaller than a nested `std::function` and makes
		// the fork-join contract explicit at this module boundary.
		// @since v0.22
		struct RangeDispatcher {
			using Body = void (*)(void *bodyContext, size_t begin, size_t end);
			using Dispatch = void (*)(void *context, size_t count, Body body, void *bodyContext);

			void *Context = nullptr;
			Dispatch Run = nullptr;
		};

		// The smallest ordinary grid rebuild where the retained radix workspace
		// and a blocking range dispatcher are considered. Pinned release samples
		// put 4096 through 16000 at serial parity or worse, while 32000 was 31%
		// faster and 64000 was 46% faster. Smaller rebuilds keep the serial path,
		// even when a dispatcher is supplied.
		// @since v0.22
		static constexpr size_t PARALLEL_MINIMUM_PROXIES = 32000;

		// Cell edge length, in metres.
		//
		// **Measured, in the `bench` preset**, over 4000 colliders whose median
		// is two metres across - `benchmarks/HashGrid.cpp`, which is also where
		// to re-take it. Four metres is the minimum of both curves that
		// matter, and they are not the same curve:
		//
		// | Cells | Rebuild, 4000 | OverlapBox | Raycast over 8m |
		// |---|---|---|---|
		// | 1 m | 893 us | 180 ns | - |
		// | 2 m | 224 us | 50 ns | 4.50 us |
		// | **4 m** | **88 us** | **33 ns** | **3.35 us** |
		// | 8 m | 61 us | 46 ns | 3.60 us |
		// | 16 m | 53 us | 79 ns | 4.50 us |
		// | 32 m | 51 us | 182 ns | - |
		//
		// The rebuild alone would choose 32 metres. It does not get to: an
		// overlap runs once per body per tick, so the thirteen nanoseconds
		// between four metres and eight are 52 microseconds over 4000 bodies,
		// and the rebuild only gives back 27. Cost per 1000 colliders to
		// rebuild is 21 us at 1000 colliders and 33 us at 16000, the
		// difference being cache rather than algorithm.
		//
		// This lands where the design note guessed - about twice the median
		// collider extent - which is worth knowing for a scene whose colliders
		// are a different size. It is a default and not a law: construct with
		// the size your own world measured, or call `SuggestCellSize` and let
		// the world measure itself.
		static constexpr float DEFAULT_CELL_SIZE = 4.0f;

		// The narrowest and widest a suggested cell may be.
		//
		// **Bounds rather than trust, because the input is a scene.** A world
		// holding one particle suggests centimetres and would bin a later
		// baseplate into a hundred million cells; a world holding one baseplate
		// suggests kilometres and puts everything in one bucket. Both are the
		// measurement being right about a scene that is about to change.
		//@{
		static constexpr float MINIMUM_CELL_SIZE = 0.25f;
		static constexpr float MAXIMUM_CELL_SIZE = 256.0f;
		//@}

		// How many cells one proxy may occupy before it stops using cells.
		//
		// A baseplate two kilometres across covers a quarter of a million cells
		// at the default spacing, and would cost a quarter of a million entries
		// and as many loop iterations every tick, for one object. Past this cap
		// a finite proxy promotes through the fixed hierarchy until it fits. The
		// coordinate clamp bounds every span before it is measured, so non-finite
		// input remains in bounded cells. Only inverted, hierarchy-exhausting, or
		// density-rejected ranges join the exact residual list.
		static constexpr size_t MAXIMUM_CELLS_PER_PROXY = 512;
		// Promoted proxies need to be materially cheaper than the base cap. The
		// hierarchy benchmark remeasures this bound against distributed terrain.
		static constexpr size_t MAXIMUM_CELLS_PER_PROMOTED_PROXY = 16;

		// Oversized finite boxes promote through five fixed grids, each eight times
		// wider than the last. The fixed shape keeps rebuild order and retained
		// storage independent of scene history. A coarse level whose busiest bucket
		// cannot separate candidates is rejected to the exact residual list.
		//
		// The valid CPU-0 bench comparison used five interleaved 15-sample pairs.
		// Normal rebuild changed +0.97%, overlap +2.86%, and short ShapeCast
		// -3.57%. The distributed rebuild plus 4000 local overlaps pipeline improved
		// -96.34%; coarse ray and ShapeCast improved -91.90% and -60.30%. Identical
		// oversized rebuild was +255.0% and grow/shrink was +71.10%, admitted costs
		// when the hierarchy cannot prune or retained storage must be reconstructed.
		static constexpr size_t HIERARCHY_LEVEL_COUNT = 5;
		static constexpr float HIERARCHY_SCALE = 8.0f;

		// Constructs an empty grid.
		//
		// @param cellSize Cell edge length in metres. A value at or below zero
		//                 is refused in favour of `DEFAULT_CELL_SIZE`, because
		//                 the alternative is a division by zero that reaches
		//                 every subsequent query as a NaN.
		explicit HashGrid(float cellSize = DEFAULT_CELL_SIZE);

		// Replaces the entire contents with `proxies`.
		//
		// The proxies are copied. Capacity is retained across calls, so a grid
		// rebuilt every tick over a steady set of objects stops allocating
		// after the first one.
		//
		// Two rebuilds from the same input produce the same iteration order.
		// That is not an accident of the implementation and is covered by a
		// test: a broad phase that visits pairs in a different order produces a
		// different solver result, and a recorded run stops replaying.
		//
		// @param proxies Everything the index should hold, in any order.
		void Rebuild(std::span<const Proxy> proxies);

		// Replaces the contents through deterministic parallel classification and
		// stable bucket radix passes when the scene is large enough for it to pay.
		//
		// The result is byte-for-byte equivalent to `Rebuild`: task order cannot
		// change a bucket's proxy or cell order. A hierarchy or scratch-cap route
		// that cannot use several logical shards rebuilds serially instead.
		//
		// @param proxies    Everything the index should hold, in any order.
		// @param dispatcher Blocking range execution supplied by the caller.
		// @since v0.22
		void RebuildParallel(std::span<const Proxy> proxies, const RangeDispatcher &dispatcher);

		// Rebuilds from proxies written straight into the grid's owned storage.
		//
		// This is for a caller that is already gathering rows and would otherwise
		// keep a second proxy vector only for `Rebuild` to copy it. `fill` is
		// called once with exactly `proxyCount` owned rows and must overwrite each
		// one before returning.
		//
		// @param proxyCount      How many proxies `fill` writes.
		// @param fill            Writes every proxy in deterministic order.
		// @param suggestCellSize Whether to size the grid from those proxies.
		// @since v0.22
		template <class Fill>
		void RebuildGenerated(size_t proxyCount, Fill &&fill, bool suggestCellSize = false);

		// Rebuilds generated proxies through the deterministic parallel route.
		//
		// @param proxyCount      How many proxies `fill` writes.
		// @param fill            Writes every proxy in deterministic order.
		// @param dispatcher      Blocking range execution supplied by the caller.
		// @param suggestCellSize Whether to size the grid from those proxies.
		// @since v0.22
		template <class Fill>
		void RebuildGeneratedParallel(
			size_t proxyCount, Fill &&fill, const RangeDispatcher &dispatcher, bool suggestCellSize = false
		);

		// Empties the grid, keeping every allocation for the next rebuild.
		void Clear();

		// Changes the cell size, emptying the grid.
		//
		// **Emptying is not a convenience, it is the only correct answer.**
		// Every entry records the cell a proxy was binned into, and those
		// coordinates are a function of the spacing - so a grid that kept its
		// entries across a size change would answer queries against cells that
		// no longer exist. The caller rebuilds; that is what a rebuild-only
		// index means.
		//
		// **A size at or below zero is refused in favour of the default**, for
		// the constructor's reason: the alternative is a division by zero that
		// reaches every subsequent query as a NaN.
		//
		// @param cellSize Cell edge length in metres.
		// @since v0.12
		void SetCellSize(float cellSize);

		// Cell edge length in metres, as resolved by the constructor.
		float CellSize() const {
			return Spacing;
		}

		// How many proxies the last rebuild put in.
		size_t ProxyCount() const {
			return Proxies.size();
		}

		// Logical storage owned by this grid.
		// @since v0.22
		HashGridStats Stats() const;

	  private:
		// The cells one proxy covers, inclusive at both ends.
		struct CellRange {
			int32_t MinimumX = 0;
			int32_t MinimumY = 0;
			int32_t MinimumZ = 0;
			int32_t MaximumX = 0;
			int32_t MaximumY = 0;
			int32_t MaximumZ = 0;
			uint8_t Level = 0;
		};

		// One proxy's membership of one cell.
		//
		// The cell is stored rather than derived from the bucket, because a
		// bucket holds every cell that hashed to it. Without the coordinate a
		// query could not tell a genuine member from a collision, and - the
		// part that actually bites - could not tell two cells of the *same*
		// proxy apart when both landed in one bucket, which would report that
		// proxy twice.
		struct Entry {
			int32_t CellX = 0;
			int32_t CellY = 0;
			int32_t CellZ = 0;
			uint32_t ProxyIndex = 0;
		};

		struct LevelStorage {
			float Spacing = DEFAULT_CELL_SIZE;
			float InverseSpacing = 1.0f / DEFAULT_CELL_SIZE;
			std::vector<uint32_t> BucketStart;
			std::vector<Entry> Entries;
		};

		float Spacing = DEFAULT_CELL_SIZE;
		float InverseSpacing = 1.0f / DEFAULT_CELL_SIZE;

		std::vector<Proxy> Proxies;
		std::vector<CellRange> Ranges;

		// One more than the bucket count: bucket `n` owns the entries in
		// `[BucketStart[n], BucketStart[n + 1])`.
		std::vector<uint32_t> BucketStart;
		std::vector<Entry> Entries;

		// Base storage stays separate because it is the ordinary scene fast path.
		// A proxy belongs to exactly one level, and these coarse arrays retain the
		// same count-prefix-fill representation without per-cell allocation.
		std::array<LevelStorage, HIERARCHY_LEVEL_COUNT - 1> CoarseLevels;
		bool HasHierarchy = false;
		bool PublishedHierarchy = false;

		// Inverted, hierarchy-exhausting, or density-rejected coarse proxies, by
		// index into `Proxies`. Every query tests all of them exactly once.
		std::vector<uint32_t> Oversized;

		// The radix path retains one temporary entry array, two key arrays, and
		// 1024 counters per logical shard. The cap is checked against retained
		// capacity before use, so a large grid falls back rather than keeping an
		// unbounded rebuild workspace.
		static constexpr size_t MAXIMUM_PARALLEL_SHARDS = 16;
		std::vector<uint32_t> ParallelBucketScratch;
		std::vector<Entry> ParallelEntryScratch;
		std::vector<uint32_t> ParallelKeys;
		std::vector<uint32_t> ParallelKeyScratch;
		std::array<size_t, MAXIMUM_PARALLEL_SHARDS> ParallelShardEntries{};
		std::array<size_t, MAXIMUM_PARALLEL_SHARDS> ParallelShardEntryStarts{};
		std::array<uint8_t, MAXIMUM_PARALLEL_SHARDS> ParallelShardBaseOnly{};
		std::array<uint16_t, 1024> ParallelHighBins{};

		void BuildIndex();
		void BuildIndexParallel(const RangeDispatcher &dispatcher);

		// The walk, the tests and the benchmark all read the arrays above, and
		// not one of them is another module. Publishing the storage to reach it
		// would turn the layout into an API somebody outside could depend on;
		// this keeps it inside `src/`, which is what the private include
		// directory is for.
		friend struct GridInternals;
	};

	// A cell size measured from the proxies that will go in the grid.
	//
	// **The rule of thumb `HashGrid::DEFAULT_CELL_SIZE` records, applied to the
	// scene instead of to the one that was benchmarked.** That default is twice
	// the median extent of *this repository's* demo colliders; a world of
	// bullets or a world of buildings gets a grid tuned for something else, and
	// `engine.physics.bench.broadphase` measures the swing at 2.7x between the
	// worst and best spacing for one scene of four thousand.
	//
	// **The mean, not the median, and the difference is a sort.** A median needs
	// the extents ordered, which is `O(n log n)` over every collider every time
	// the set changes; the mean is one pass. What a median buys is resistance to
	// outliers - and the quantisation below already flattens those, because an
	// outlier has to move the mean by a factor of two before it moves the
	// answer at all.
	//
	// **Quantised to a power of two, which is the hysteresis.** A size computed
	// exactly would differ every time a body was added, and each difference
	// costs a full rebuild of the index; rounding to a power of two means the
	// answer only changes when the scene's scale genuinely does. It is also
	// where the cost curve is flat - the benchmark's 8 m and 16 m rows are
	// within nine per cent of each other.
	//
	// **Deterministic, which it has to be.** One pass in the caller's order over
	// values that are already in the store, no clock and no address. Two runs of
	// one scene choose one size, and `just determinism` is what would report it
	// if they did not.
	//
	// @param proxies What the grid is about to hold.
	// @return A cell size in `[MINIMUM_CELL_SIZE, MAXIMUM_CELL_SIZE]`, or
	//         `HashGrid::DEFAULT_CELL_SIZE` for an empty set.
	// @since v0.12
	float SuggestCellSize(std::span<const Proxy> proxies);

	template <class Fill>
	void HashGrid::RebuildGenerated(size_t proxyCount, Fill &&fill, bool suggestCellSize) {
		Proxies.resize(proxyCount);
		std::forward<Fill>(fill)(std::span<Proxy>(Proxies));

		if (suggestCellSize) {
			const float suggested = SuggestCellSize(Proxies);
			if (suggested != Spacing) {
				Spacing = suggested;
				InverseSpacing = 1.0f / suggested;
			}
		}

		BuildIndex();
	}

	template <class Fill>
	void HashGrid::RebuildGeneratedParallel(
		size_t proxyCount, Fill &&fill, const RangeDispatcher &dispatcher, bool suggestCellSize
	) {
		Proxies.resize(proxyCount);
		std::forward<Fill>(fill)(std::span<Proxy>(Proxies));

		if (suggestCellSize) {
			const float suggested = SuggestCellSize(Proxies);
			if (suggested != Spacing) {
				Spacing = suggested;
				InverseSpacing = 1.0f / suggested;
			}
		}

		BuildIndexParallel(dispatcher);
	}
}
