#pragma once

// The grid's cell arithmetic and its candidate walk. Private to this module.
//
// Everything here needs the storage `HashGrid` keeps to itself, which is why it
// is a friend rather than a set of public methods: the queries in `Query.cpp`,
// the suites in `tests/` and the benchmark are all inside this module, and none
// of them is a reason to publish a layout.
//
// The two pieces of arithmetic are here rather than in `HashGrid.cpp` because
// the walk and the build have to agree about them exactly. A cell coordinate
// computed one way in the build and another way in the query is a proxy that is
// stored in one cell and looked for in another, and it produces a broad phase
// that misses about one contact in a thousand.

#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::spatial {

	// How far a cell coordinate may travel from the origin.
	//
	// Two purposes, and the second is the load-bearing one. It keeps the cube
	// of a cell span inside a signed 64-bit count so the size of a proxy's cell
	// range cannot overflow the arithmetic that decides whether it is
	// oversized; and it makes casting the coordinate defined for an infinite or
	// NaN bound, which is otherwise undefined behaviour and in practice a
	// negative cell index far from anything.
	//
	// At the default spacing this is a quarter of a million cells, or about a
	// million metres, in each direction from the origin — well past the point
	// where a float position has metre resolution at all.
	inline constexpr int32_t CELL_LIMIT = 1 << 18;

	// How many cells a query may walk beyond what the grid actually holds
	// before scanning every proxy instead.
	//
	// A query volume covering more cells than there are entries in the whole
	// grid is asking the index to do more work than the answer is worth: past
	// that point one pass over every proxy is both cheaper and bounded. The
	// allowance on top is what keeps an ordinary query on a nearly empty grid
	// on the fast path, where otherwise a four-cell query against a one-entry
	// grid would take the scan.
	inline constexpr int64_t WALK_CELL_ALLOWANCE = 4096;

	// The cell index one coordinate falls in.
	//
	// **`std::floor`, not a cast.** A cast truncates toward zero, which puts
	// -0.5 and +0.5 in the same cell and mirrors every cell boundary about the
	// origin. Nothing written with positive coordinates ever notices, which is
	// exactly what makes it expensive: the first bug report is a character
	// falling through the floor on one side of the map.
	//
	// @param value           A world coordinate on one axis.
	// @param inverseSpacing  One divided by the cell size, computed once.
	inline int32_t CellCoordinateOf(float value, float inverseSpacing) {
		const float cell = std::floor(value * inverseSpacing);

		// Written as a pair of rejections rather than a clamp so that NaN —
		// which compares false against everything — lands on the lower limit
		// instead of being cast, which is undefined.
		if (!(cell > static_cast<float>(-CELL_LIMIT))) {
			return -CELL_LIMIT;
		}
		if (cell > static_cast<float>(CELL_LIMIT)) {
			return CELL_LIMIT;
		}
		return static_cast<int32_t>(cell);
	}

	// Hashes a cell coordinate.
	//
	// The three constants are the usual large primes for this. The arithmetic
	// is on `uint32_t` deliberately: the multiply is meant to overflow, and
	// unsigned overflow is defined to wrap where signed overflow is undefined.
	//
	// A collision costs a candidate that is rejected on the next line. It can
	// never cost a miss, because a cell always hashes to the same bucket in the
	// build and in the query.
	inline uint32_t HashCell(int32_t cellX, int32_t cellY, int32_t cellZ) {
		const uint32_t x = static_cast<uint32_t>(cellX) * 73856093u;
		const uint32_t y = static_cast<uint32_t>(cellY) * 19349663u;
		const uint32_t z = static_cast<uint32_t>(cellZ) * 83492791u;
		return x ^ y ^ z;
	}

	// Reaches the grid's storage, and walks it.
	struct GridInternals {
		// How many bucket entries the last rebuild produced.
		static size_t EntryCount(const HashGrid &grid) {
			return grid.Entries.size();
		}

		// How much entry storage is retained. What a test observes to show that
		// a second rebuild reused the first's allocation.
		static size_t EntryCapacity(const HashGrid &grid) {
			return grid.Entries.capacity();
		}

		// Where the entry storage lives. Unchanged across a rebuild means the
		// vector did not reallocate, which is the stronger half of the claim.
		static const void *EntryData(const HashGrid &grid) {
			return grid.Entries.data();
		}

		// How many buckets the last rebuild chose. Always a power of two.
		static size_t BucketCount(const HashGrid &grid) {
			return grid.BucketStart.empty() ? 0 : grid.BucketStart.size() - 1;
		}

		// Which bucket a cell lands in, for a test that needs to force a collision.
		static size_t BucketOf(const HashGrid &grid, int32_t cellX, int32_t cellY, int32_t cellZ) {
			return HashCell(cellX, cellY, cellZ) & (BucketCount(grid) - 1);
		}

		// How many proxies were too large for cells.
		static size_t OversizedCount(const HashGrid &grid) {
			return grid.Oversized.size();
		}

		// Calls `visit` once for each proxy whose box overlaps `volume` and
		// whose layers overlap `mask`.
		//
		// **Once**, not once per shared cell. A proxy spanning several cells is
		// reported from the first cell of the walk that lies in both its own
		// cell range and the query's — which, because every axis is walked
		// ascending, is the corner formed by taking the larger minimum on each
		// axis, and is therefore an O(1) comparison with no scratch memory.
		//
		// The alternative, a per-proxy visited stamp, makes a query a *write*
		// and ends any possibility of two threads querying one grid at once.
		// It also gives an answer that depends on which thread got there first.
		//
		// @param visit Called with each candidate. Returning false stops the
		//              walk, which is what a full output span wants.
		// @return False if the visitor stopped the walk.
		template <class Visit>
		static bool
		ForEachCandidate(const HashGrid &grid, const core::AABB &volume, LayerMask mask, Visit &&visit) {
			const int32_t minimumX = CellCoordinateOf(volume.Minimum.X, grid.InverseSpacing);
			const int32_t minimumY = CellCoordinateOf(volume.Minimum.Y, grid.InverseSpacing);
			const int32_t minimumZ = CellCoordinateOf(volume.Minimum.Z, grid.InverseSpacing);
			const int32_t maximumX = CellCoordinateOf(volume.Maximum.X, grid.InverseSpacing);
			const int32_t maximumY = CellCoordinateOf(volume.Maximum.Y, grid.InverseSpacing);
			const int32_t maximumZ = CellCoordinateOf(volume.Maximum.Z, grid.InverseSpacing);

			// A volume built the wrong way round covers no cells. It can still
			// satisfy AABB::Overlaps against a large enough box, so the scan
			// below is what answers it rather than an empty walk.
			const bool inverted = maximumX < minimumX || maximumY < minimumY || maximumZ < minimumZ;

			const int64_t cells = inverted ? 0
										   : (static_cast<int64_t>(maximumX) - minimumX + 1) *
												 (static_cast<int64_t>(maximumY) - minimumY + 1) *
												 (static_cast<int64_t>(maximumZ) - minimumZ + 1);

			// An empty grid has no buckets to mask against, so it takes the
			// scan — which still has to run, because a proxy too large for
			// cells produces no entries and is found nowhere else.
			if (inverted || grid.Entries.empty() ||
				cells > static_cast<int64_t>(grid.Entries.size()) + WALK_CELL_ALLOWANCE) {
				return ScanEveryProxy(grid, volume, mask, visit);
			}

			for (int32_t cellZ = minimumZ; cellZ <= maximumZ; cellZ++) {
				for (int32_t cellY = minimumY; cellY <= maximumY; cellY++) {
					for (int32_t cellX = minimumX; cellX <= maximumX; cellX++) {
						const size_t bucket = HashCell(cellX, cellY, cellZ) & (BucketCount(grid) - 1);
						const uint32_t last = grid.BucketStart[bucket + 1];

						for (uint32_t slot = grid.BucketStart[bucket]; slot < last; slot++) {
							const HashGrid::Entry &entry = grid.Entries[slot];

							// A bucket holds every cell that hashed to it, so
							// this is where a collision is rejected. Cheaper
							// than the box test below and it does a different
							// job: this one says "not this cell", the box test
							// says "not this volume".
							if (entry.CellX != cellX || entry.CellY != cellY || entry.CellZ != cellZ) {
								continue;
							}

							const HashGrid::CellRange &range = grid.Ranges[entry.ProxyIndex];
							if (cellX != std::max(range.MinimumX, minimumX) ||
								cellY != std::max(range.MinimumY, minimumY) ||
								cellZ != std::max(range.MinimumZ, minimumZ)) {
								continue;
							}

							const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
							if (!proxy.Layers.Overlaps(mask) || !proxy.Bounds.Overlaps(volume)) {
								continue;
							}
							if (!visit(proxy)) {
								return false;
							}
						}
					}
				}
			}

			// After the cells and in proxy order, so the whole walk stays one
			// fixed sequence for one input.
			for (uint32_t index : grid.Oversized) {
				const Proxy &proxy = grid.Proxies[index];
				if (!proxy.Layers.Overlaps(mask) || !proxy.Bounds.Overlaps(volume)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}

	  private:
		// Every proxy, in order, with no cells involved. The answer for a query
		// so large that the walk would cost more than the scan.
		template <class Visit>
		static bool
		ScanEveryProxy(const HashGrid &grid, const core::AABB &volume, LayerMask mask, Visit &&visit) {
			for (const Proxy &proxy : grid.Proxies) {
				if (!proxy.Layers.Overlaps(mask) || !proxy.Bounds.Overlaps(volume)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}
	};
}
