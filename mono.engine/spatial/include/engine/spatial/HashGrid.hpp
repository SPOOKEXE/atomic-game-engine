#pragma once

// A uniform hash grid over boxes, rebuilt from scratch every time.
//
// **The grid is rebuilt, never edited.** There is no `Insert`, no `Move` and no
// `Remove`, and adding one would not be an extension — it would be a different
// data structure. This one is count-then-fill: one pass to measure, a prefix
// sum, one pass to place every proxy into a single flat array, with no per-cell
// vector and no allocation once the arrays have reached their size. An editable
// grid needs a per-cell list with holes in it, which allocates, fragments, and
// iterates in an order that depends on the history of the edits rather than on
// the contents. `v02v03v04.md` describes both in different paragraphs; the
// allocation table is the one written as a standing rule, so this is that one.
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

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::spatial {

	// One thing the index knows about.
	//
	// The layers are **in the proxy** rather than looked up per candidate. A
	// grid that held only `{id, box}` would have to resolve every candidate
	// through whatever owns the layers to filter it — one random access per
	// candidate, which is precisely the cost an index exists to remove — and it
	// would make this module depend on the ECS to do it.
	//
	// @since v0.4
	struct Proxy {
		// What the caller calls this. Copied through to a hit unchanged.
		uint64_t Id = 0;

		// Its world-space box. The index tests against this and nothing finer;
		// a caller wanting the exact shape tests the candidates itself.
		core::AABB Bounds;

		// Which layers it is on. Empty by default, matching nothing — see
		// `LayerMask`, which explains why that is the safer of the two
		// wrong-by-default answers.
		LayerMask Layers;
	};

	// A uniform grid of cells, hashed into a fixed number of buckets.
	//
	// Build one per set of proxies that move together, call `Rebuild` with all
	// of them, then query it. It holds a copy of the proxies, because every
	// candidate a bucket produces is re-tested against its own box and the test
	// needs the box.
	//
	// @since v0.4
	class HashGrid {
	  public:
		// Cell edge length, in metres.
		//
		// **Measured, in the `bench` preset**, over 4000 colliders whose median
		// is two metres across — `benchmarks/HashGrid.cpp`, which is also where
		// to re-take it. Four metres is the minimum of both curves that
		// matter, and they are not the same curve:
		//
		// | Cells | Rebuild, 4000 | OverlapBox | Raycast over 8m |
		// |---|---|---|---|
		// | 1 m | 893 us | 180 ns | — |
		// | 2 m | 224 us | 50 ns | 4.50 us |
		// | **4 m** | **88 us** | **33 ns** | **3.35 us** |
		// | 8 m | 61 us | 46 ns | 3.60 us |
		// | 16 m | 53 us | 79 ns | 4.50 us |
		// | 32 m | 51 us | 182 ns | — |
		//
		// The rebuild alone would choose 32 metres. It does not get to: an
		// overlap runs once per body per tick, so the thirteen nanoseconds
		// between four metres and eight are 52 microseconds over 4000 bodies,
		// and the rebuild only gives back 27. Cost per 1000 colliders to
		// rebuild is 21 us at 1000 colliders and 33 us at 16000, the
		// difference being cache rather than algorithm.
		//
		// This lands where the design note guessed — about twice the median
		// collider extent — which is worth knowing for a scene whose colliders
		// are a different size. It is a default and not a law: construct with
		// the size your own world measured.
		static constexpr float DEFAULT_CELL_SIZE = 4.0f;

		// How many cells one proxy may occupy before it stops using cells.
		//
		// A baseplate two kilometres across covers a quarter of a million cells
		// at the default spacing, and would cost a quarter of a million entries
		// and as many loop iterations every tick, for one object. Past this cap
		// a proxy goes on a short list that every query examines directly,
		// which is bounded, exact, and cheaper than either alternative. It is
		// also what keeps an infinite or NaN box from asking for an unbounded
		// allocation.
		static constexpr size_t MAXIMUM_CELLS_PER_PROXY = 512;

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

		// Empties the grid, keeping every allocation for the next rebuild.
		void Clear();

		// Cell edge length in metres, as resolved by the constructor.
		float CellSize() const {
			return Spacing;
		}

		// How many proxies the last rebuild put in.
		size_t ProxyCount() const {
			return Proxies.size();
		}

	  private:
		// The cells one proxy covers, inclusive at both ends.
		struct CellRange {
			int32_t MinimumX = 0;
			int32_t MinimumY = 0;
			int32_t MinimumZ = 0;
			int32_t MaximumX = 0;
			int32_t MaximumY = 0;
			int32_t MaximumZ = 0;
		};

		// One proxy's membership of one cell.
		//
		// The cell is stored rather than derived from the bucket, because a
		// bucket holds every cell that hashed to it. Without the coordinate a
		// query could not tell a genuine member from a collision, and — the
		// part that actually bites — could not tell two cells of the *same*
		// proxy apart when both landed in one bucket, which would report that
		// proxy twice.
		struct Entry {
			int32_t CellX = 0;
			int32_t CellY = 0;
			int32_t CellZ = 0;
			uint32_t ProxyIndex = 0;
		};

		float Spacing = DEFAULT_CELL_SIZE;
		float InverseSpacing = 1.0f / DEFAULT_CELL_SIZE;

		std::vector<Proxy> Proxies;
		std::vector<CellRange> Ranges;

		// One more than the bucket count: bucket `n` owns the entries in
		// `[BucketStart[n], BucketStart[n + 1])`.
		std::vector<uint32_t> BucketStart;
		std::vector<uint32_t> BucketCursor;
		std::vector<Entry> Entries;

		// Proxies too large for cells, by index into `Proxies`. Every query
		// tests all of them.
		std::vector<uint32_t> Oversized;

		// The walk, the tests and the benchmark all read the arrays above, and
		// not one of them is another module. Publishing the storage to reach it
		// would turn the layout into an API somebody outside could depend on;
		// this keeps it inside `src/`, which is what the private include
		// directory is for.
		friend struct GridInternals;
	};
}
