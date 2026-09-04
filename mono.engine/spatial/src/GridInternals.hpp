#pragma once

// The grid's cell arithmetic and its three candidate walks. Private to this
// module.
//
// There are three walks because there are three shapes of question. A volume
// query visits a box, a ray query visits a line, and a swept-box query visits a
// line with thickness. They de-duplicate a proxy spanning several cells by
// rules written against their own traversal. See the comment on each.
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

#include "RayBox.hpp"

#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

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
	// million metres, in each direction from the origin - well past the point
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

		// Written as a pair of rejections rather than a clamp so that NaN -
		// which compares false against everything - lands on the lower limit
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

		// The exact bucket prefix array. Tests compare this and `EntryBytes` to
		// prove a dispatched rebuild preserved the serial layout, not merely the
		// query set it happens to produce.
		static std::span<const uint32_t> BucketStarts(const HashGrid &grid) {
			return grid.BucketStart;
		}

		static std::span<const std::byte> EntryBytes(const HashGrid &grid) {
			return std::as_bytes(std::span<const HashGrid::Entry>(grid.Entries));
		}

		static size_t ParallelScratchCapacity(const HashGrid &grid) {
			return grid.ParallelBucketScratch.capacity() * sizeof(uint32_t) +
				   grid.ParallelEntryScratch.capacity() * sizeof(HashGrid::Entry) +
				   grid.ParallelKeys.capacity() * sizeof(uint32_t) +
				   grid.ParallelKeyScratch.capacity() * sizeof(uint32_t);
		}

		static size_t ParallelScratchBytes(const HashGrid &grid) {
			return grid.ParallelBucketScratch.size() * sizeof(uint32_t) +
				   grid.ParallelEntryScratch.size() * sizeof(HashGrid::Entry) +
				   grid.ParallelKeys.size() * sizeof(uint32_t) +
				   grid.ParallelKeyScratch.size() * sizeof(uint32_t);
		}

		// Which bucket a cell lands in, for a test that needs to force a collision.
		static size_t BucketOf(const HashGrid &grid, int32_t cellX, int32_t cellY, int32_t cellZ) {
			return HashCell(cellX, cellY, cellZ) & (BucketCount(grid) - 1);
		}

		// How many inverted, hierarchy-exhausting, or density-rejected proxies need
		// the residual scan.
		static size_t OversizedCount(const HashGrid &grid) {
			return grid.Oversized.size();
		}

		static size_t LevelProxyCount(const HashGrid &grid, size_t level) {
			return static_cast<size_t>(std::count_if(
				grid.Ranges.begin(), grid.Ranges.end(), [level](const HashGrid::CellRange &range) {
					return range.MaximumX >= range.MinimumX && range.Level == level;
				}
			));
		}

		static size_t RetainedHierarchyBytes(const HashGrid &grid) {
			size_t bytes = grid.Proxies.capacity() * sizeof(Proxy) +
						   grid.Ranges.capacity() * sizeof(HashGrid::CellRange) +
						   grid.BucketStart.capacity() * sizeof(uint32_t) +
						   grid.Entries.capacity() * sizeof(HashGrid::Entry) +
						   grid.Oversized.capacity() * sizeof(uint32_t) + ParallelScratchCapacity(grid);
			for (const HashGrid::LevelStorage &level : grid.CoarseLevels) {
				bytes += level.BucketStart.capacity() * sizeof(uint32_t) +
						 level.Entries.capacity() * sizeof(HashGrid::Entry);
			}
			return bytes;
		}

		// Calls `visit` once for each proxy whose box overlaps `volume` and
		// whose layers overlap `mask`.
		//
		// **Once**, not once per shared cell. A proxy spanning several cells is
		// reported from the first cell of the walk that lies in both its own
		// cell range and the query's - which, because every axis is walked
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
		template <class Accept, class Visit>
		static bool ForEachCandidateAccepted(
			const HashGrid &grid, const core::AABB &volume, LayerMask mask, Accept &&accept, Visit &&visit
		) {
			if (!grid.HasHierarchy) {
				const int32_t minimumX = CellCoordinateOf(volume.Minimum.X, grid.InverseSpacing);
				const int32_t minimumY = CellCoordinateOf(volume.Minimum.Y, grid.InverseSpacing);
				const int32_t minimumZ = CellCoordinateOf(volume.Minimum.Z, grid.InverseSpacing);
				const int32_t maximumX = CellCoordinateOf(volume.Maximum.X, grid.InverseSpacing);
				const int32_t maximumY = CellCoordinateOf(volume.Maximum.Y, grid.InverseSpacing);
				const int32_t maximumZ = CellCoordinateOf(volume.Maximum.Z, grid.InverseSpacing);
				const bool inverted = maximumX < minimumX || maximumY < minimumY || maximumZ < minimumZ;
				const int64_t cells =
					CellsInRange(minimumX, minimumY, minimumZ, maximumX, maximumY, maximumZ);
				if (inverted || grid.Entries.empty() ||
					cells > static_cast<int64_t>(grid.Entries.size()) + WALK_CELL_ALLOWANCE) {
					return ScanEveryProxyAccepted(grid, volume, mask, accept, visit);
				}

				for (int32_t cellZ = minimumZ; cellZ <= maximumZ; cellZ++) {
					for (int32_t cellY = minimumY; cellY <= maximumY; cellY++) {
						for (int32_t cellX = minimumX; cellX <= maximumX; cellX++) {
							const size_t bucket = HashCell(cellX, cellY, cellZ) & (BucketCount(grid) - 1);
							for (uint32_t slot = grid.BucketStart[bucket];
								 slot < grid.BucketStart[bucket + 1];
								 slot++) {
								const HashGrid::Entry &entry = grid.Entries[slot];
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
								if (!accept(proxy) || !proxy.Layers.Overlaps(mask) ||
									!proxy.Bounds.Overlaps(volume)) {
									continue;
								}
								if (!visit(proxy)) {
									return false;
								}
							}
						}
					}
				}
				return true;
			}

			auto needsScan = [&](float inverseSpacing, const std::vector<HashGrid::Entry> &entries) {
				if (entries.empty()) {
					return false;
				}
				const int32_t minimumX = CellCoordinateOf(volume.Minimum.X, inverseSpacing);
				const int32_t minimumY = CellCoordinateOf(volume.Minimum.Y, inverseSpacing);
				const int32_t minimumZ = CellCoordinateOf(volume.Minimum.Z, inverseSpacing);
				const int32_t maximumX = CellCoordinateOf(volume.Maximum.X, inverseSpacing);
				const int32_t maximumY = CellCoordinateOf(volume.Maximum.Y, inverseSpacing);
				const int32_t maximumZ = CellCoordinateOf(volume.Maximum.Z, inverseSpacing);
				const int64_t cells =
					CellsInRange(minimumX, minimumY, minimumZ, maximumX, maximumY, maximumZ);
				return maximumX < minimumX || maximumY < minimumY || maximumZ < minimumZ ||
					   cells > static_cast<int64_t>(entries.size()) + WALK_CELL_ALLOWANCE;
			};

			if (needsScan(grid.InverseSpacing, grid.Entries)) {
				return ScanEveryProxyAccepted(grid, volume, mask, accept, visit);
			}
			if (grid.HasHierarchy) {
				for (const HashGrid::LevelStorage &level : grid.CoarseLevels) {
					if (needsScan(level.InverseSpacing, level.Entries)) {
						return ScanEveryProxyAccepted(grid, volume, mask, accept, visit);
					}
				}
			}

			auto walkLevel = [&](size_t levelIndex,
								 float inverseSpacing,
								 const std::vector<uint32_t> &bucketStart,
								 const std::vector<HashGrid::Entry> &entries) {
				if (entries.empty()) {
					return true;
				}
				const int32_t minimumX = CellCoordinateOf(volume.Minimum.X, inverseSpacing);
				const int32_t minimumY = CellCoordinateOf(volume.Minimum.Y, inverseSpacing);
				const int32_t minimumZ = CellCoordinateOf(volume.Minimum.Z, inverseSpacing);
				const int32_t maximumX = CellCoordinateOf(volume.Maximum.X, inverseSpacing);
				const int32_t maximumY = CellCoordinateOf(volume.Maximum.Y, inverseSpacing);
				const int32_t maximumZ = CellCoordinateOf(volume.Maximum.Z, inverseSpacing);
				const size_t bucketCount = bucketStart.size() - 1;

				for (int32_t cellZ = minimumZ; cellZ <= maximumZ; cellZ++) {
					for (int32_t cellY = minimumY; cellY <= maximumY; cellY++) {
						for (int32_t cellX = minimumX; cellX <= maximumX; cellX++) {
							const size_t bucket = HashCell(cellX, cellY, cellZ) & (bucketCount - 1);
							for (uint32_t slot = bucketStart[bucket]; slot < bucketStart[bucket + 1];
								 slot++) {
								const HashGrid::Entry &entry = entries[slot];
								if (entry.CellX != cellX || entry.CellY != cellY || entry.CellZ != cellZ) {
									continue;
								}

								const HashGrid::CellRange &range = grid.Ranges[entry.ProxyIndex];
								if (range.Level != levelIndex ||
									cellX != std::max(range.MinimumX, minimumX) ||
									cellY != std::max(range.MinimumY, minimumY) ||
									cellZ != std::max(range.MinimumZ, minimumZ)) {
									continue;
								}

								const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
								if (!accept(proxy) || !proxy.Layers.Overlaps(mask) ||
									!proxy.Bounds.Overlaps(volume)) {
									continue;
								}
								if (!visit(proxy)) {
									return false;
								}
							}
						}
					}
				}
				return true;
			};

			if (!walkLevel(0, grid.InverseSpacing, grid.BucketStart, grid.Entries)) {
				return false;
			}
			if (grid.HasHierarchy) {
				for (size_t level = 1; level < HashGrid::HIERARCHY_LEVEL_COUNT; level++) {
					const HashGrid::LevelStorage &storage = grid.CoarseLevels[level - 1];
					if (!walkLevel(level, storage.InverseSpacing, storage.BucketStart, storage.Entries)) {
						return false;
					}
				}
			}

			// After the cells and in proxy order, so the whole walk stays one
			// fixed sequence for one input.
			for (uint32_t index : grid.Oversized) {
				const Proxy &proxy = grid.Proxies[index];
				if (!accept(proxy) || !proxy.Layers.Overlaps(mask) || !proxy.Bounds.Overlaps(volume)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}

		template <class Visit>
		static bool
		ForEachCandidate(const HashGrid &grid, const core::AABB &volume, LayerMask mask, Visit &&visit) {
			return ForEachCandidateAccepted(
				grid, volume, mask, [](const Proxy &) { return true; }, std::forward<Visit>(visit)
			);
		}

		template <class Visit>
		static bool ForEachCandidateAfterId(
			const HashGrid &grid,
			const core::AABB &volume,
			LayerMask mask,
			uint64_t minimumExclusive,
			Visit &&visit
		) {
			return ForEachCandidateAccepted(
				grid,
				volume,
				mask,
				[minimumExclusive](const Proxy &proxy) { return proxy.Id > minimumExclusive; },
				std::forward<Visit>(visit)
			);
		}

		// Calls `visit` once for each proxy in a cell the ray actually pierces,
		// in increasing distance along the ray.
		//
		// **A digital differential walk - Amanatides and Woo, 1987.** The box
		// walk above is given the bounding box of the whole segment, so a ray
		// crossing a room diagonally asks for the cube that contains it: 25
		// cells on a side is 15,625 cells to reach the 75 the line goes through.
		// This steps cell to cell along the line instead, so the count is the
		// ray's length in cells rather than its bounding volume in cells.
		//
		// **The measurement `AGENTS.md` requires of an algorithmic change**,
		// from `benchmarks/HashGrid.cpp` on the `bench` preset, 64 rays of 64 m
		// against 4000 colliders, before and after:
		//
		//     cells    box walk    ray walk
		//      1 m     10.67 ms    12.90 us
		//      2 m      1.78 ms     7.50 us
		//      4 m    422.76 us     5.25 us
		//      8 m    114.80 us     5.18 us
		//     16 m     26.40 us     7.09 us
		//     32 m     32.25 us    15.70 us
		//
		// The shape is the argument, not the ratio. The box walk's cost is
		// cubic in the reciprocal of the cell size and this one's is linear, so
		// the two agree at coarse cells and diverge without limit at fine ones -
		// which is why the old default could not be chosen from the raycast rows
		// and this one can. A short cast is barely changed: 64 rays of 8 m at
		// 4 m cells go 3.13 us to 2.50 us, because a short ray's bounding box
		// was never the problem.
		//
		// **De-duplication is "the cell before this one was not in the proxy's
		// range", and that is the rule change the walk came with.** The box
		// walk's rule - report from the corner formed by the larger minimum on
		// each axis - needs a walk that covers a whole box, and a line does not.
		// This one works because a straight ray is monotone on every axis, so
		// the cells it visits inside any box in cell space form one unbroken
		// run: the test fires on the first cell of that run and nowhere else.
		// One comparison, no scratch memory, no write, and still the same answer
		// whatever order anything runs in - which is what `AGENTS.md` requires
		// of it and what a visited stamp would have cost.
		//
		// **No volume test on a candidate.** The box walk rejects a proxy whose
		// cell overlaps the query while its box does not; here the caller's own
		// exact ray-box test answers that question better, so repeating a
		// coarser version of it first is work with no result.
		//
		// @param reciprocal  `RayReciprocal` for the same ray, which the caller
		//                    already holds for its exact test.
		// @param visit       Called with each candidate. Returning false stops
		//                    the walk.
		// @param keepWalking Called with the distance at which the ray leaves
		//                    the cell just visited, before stepping to the next.
		//                    Returning false stops the cell walk, which is how a
		//                    nearest-hit query stops once nothing further along
		//                    can beat what it holds. The residual pass still
		//                    runs, because a hierarchy-exhausting proxy is in no
		//                    level and the walks never reached it.
		// @return False if the visitor stopped the walk.
		template <class Visit, class KeepWalking>
		static bool ForEachCandidateAlongRay(
			const HashGrid &grid,
			const core::Ray &ray,
			const RayReciprocal &reciprocal,
			float maxDistance,
			LayerMask mask,
			Visit &&visit,
			KeepWalking &&keepWalking
		) {
			const float origin[3] = {ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
			const float direction[3] = {ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
			if (!grid.HasHierarchy) {
				const int32_t cell[3] = {
					CellCoordinateOf(origin[0], grid.InverseSpacing),
					CellCoordinateOf(origin[1], grid.InverseSpacing),
					CellCoordinateOf(origin[2], grid.InverseSpacing),
				};
				double crossings = 3.0;
				for (int axis = 0; axis < 3; axis++) {
					crossings += static_cast<double>(std::abs(direction[axis])) *
									 static_cast<double>(maxDistance) *
									 static_cast<double>(grid.InverseSpacing) +
								 1.0;
				}
				const bool clamped = std::abs(cell[0]) >= CELL_LIMIT || std::abs(cell[1]) >= CELL_LIMIT ||
									 std::abs(cell[2]) >= CELL_LIMIT;
				if (grid.Entries.empty() || clamped ||
					!(crossings <= static_cast<double>(grid.Entries.size()) + WALK_CELL_ALLOWANCE)) {
					return ScanEveryProxy(grid, SegmentBounds(ray, maxDistance), mask, visit);
				}
				return ForEachCellAlongRay(
					grid.Spacing,
					grid.InverseSpacing,
					ray,
					reciprocal,
					maxDistance,
					[&](const int32_t (&current)[3], const int32_t (&previous)[3], bool stepped) {
						const size_t bucket =
							HashCell(current[0], current[1], current[2]) & (BucketCount(grid) - 1);
						for (uint32_t slot = grid.BucketStart[bucket]; slot < grid.BucketStart[bucket + 1];
							 slot++) {
							const HashGrid::Entry &entry = grid.Entries[slot];
							if (entry.CellX != current[0] || entry.CellY != current[1] ||
								entry.CellZ != current[2]) {
								continue;
							}
							if (stepped && WithinCellRange(grid.Ranges[entry.ProxyIndex], previous)) {
								continue;
							}
							const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
							if (!proxy.Layers.Overlaps(mask)) {
								continue;
							}
							if (!visit(proxy)) {
								return false;
							}
						}
						return true;
					},
					keepWalking
				);
			}

			int32_t cell[3] = {
				CellCoordinateOf(origin[0], grid.InverseSpacing),
				CellCoordinateOf(origin[1], grid.InverseSpacing),
				CellCoordinateOf(origin[2], grid.InverseSpacing),
			};

			// How many cells the line crosses, which is its extent on each axis
			// in cells plus the cell it starts in. In `double` so that a long ray
			// on a fine grid cannot overflow the count that decides whether to
			// walk at all, and written as `!(<=)` so a NaN takes the scan.
			double crossings = 3.0;
			for (int axis = 0; axis < 3; axis++) {
				crossings += static_cast<double>(std::abs(direction[axis])) *
								 static_cast<double>(maxDistance) * static_cast<double>(grid.InverseSpacing) +
							 1.0;
			}

			// An origin clamped at the coordinate limit is an origin whose cell
			// is not the cell it is in, and stepping from it would walk a line
			// that is not the ray's. The scan answers it exactly instead.
			const bool clamped = std::abs(cell[0]) >= CELL_LIMIT || std::abs(cell[1]) >= CELL_LIMIT ||
								 std::abs(cell[2]) >= CELL_LIMIT;

			if (clamped || (!grid.Entries.empty() &&
							!(crossings <= static_cast<double>(grid.Entries.size()) + WALK_CELL_ALLOWANCE))) {
				return ScanEveryProxy(grid, SegmentBounds(ray, maxDistance), mask, visit);
			}
			for (const HashGrid::LevelStorage &storage : grid.CoarseLevels) {
				if (storage.Entries.empty()) {
					continue;
				}
				const int32_t coarseCell[3] = {
					CellCoordinateOf(origin[0], storage.InverseSpacing),
					CellCoordinateOf(origin[1], storage.InverseSpacing),
					CellCoordinateOf(origin[2], storage.InverseSpacing),
				};
				double coarseCrossings = 3.0;
				for (int axis = 0; axis < 3; axis++) {
					coarseCrossings += static_cast<double>(std::abs(direction[axis])) *
										   static_cast<double>(maxDistance) *
										   static_cast<double>(storage.InverseSpacing) +
									   1.0;
				}
				const bool coarseClamped = std::abs(coarseCell[0]) >= CELL_LIMIT ||
										   std::abs(coarseCell[1]) >= CELL_LIMIT ||
										   std::abs(coarseCell[2]) >= CELL_LIMIT;
				if (coarseClamped ||
					!(coarseCrossings <= static_cast<double>(storage.Entries.size()) + WALK_CELL_ALLOWANCE)) {
					return ScanEveryProxy(grid, SegmentBounds(ray, maxDistance), mask, visit);
				}
			}

			if (!grid.Entries.empty() &&
				!ForEachCellAlongRay(
					grid.Spacing,
					grid.InverseSpacing,
					ray,
					reciprocal,
					maxDistance,
					[&](const int32_t (&current)[3], const int32_t (&previous)[3], bool stepped) {
						const size_t bucket =
							HashCell(current[0], current[1], current[2]) & (BucketCount(grid) - 1);
						const uint32_t last = grid.BucketStart[bucket + 1];

						for (uint32_t slot = grid.BucketStart[bucket]; slot < last; slot++) {
							const HashGrid::Entry &entry = grid.Entries[slot];
							if (entry.CellX != current[0] || entry.CellY != current[1] ||
								entry.CellZ != current[2]) {
								continue;
							}

							if (stepped && WithinCellRange(grid.Ranges[entry.ProxyIndex], previous)) {
								continue;
							}

							const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
							if (!proxy.Layers.Overlaps(mask)) {
								continue;
							}
							if (!visit(proxy)) {
								return false;
							}
						}
						return true;
					},
					keepWalking
				)) {
				return false;
			}

			for (size_t level = 1; level < HashGrid::HIERARCHY_LEVEL_COUNT; level++) {
				const HashGrid::LevelStorage &storage = grid.CoarseLevels[level - 1];
				if (storage.Entries.empty()) {
					continue;
				}

				if (!ForEachCellAlongRay(
						storage.Spacing,
						storage.InverseSpacing,
						ray,
						reciprocal,
						maxDistance,
						[&](const int32_t (&current)[3], const int32_t (&previous)[3], bool stepped) {
							const size_t bucket = HashCell(current[0], current[1], current[2]) &
												  (storage.BucketStart.size() - 2);
							for (uint32_t slot = storage.BucketStart[bucket];
								 slot < storage.BucketStart[bucket + 1];
								 slot++) {
								const HashGrid::Entry &entry = storage.Entries[slot];
								if (entry.CellX != current[0] || entry.CellY != current[1] ||
									entry.CellZ != current[2]) {
									continue;
								}
								const HashGrid::CellRange &range = grid.Ranges[entry.ProxyIndex];
								if (range.Level != level || (stepped && WithinCellRange(range, previous))) {
									continue;
								}
								const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
								if (proxy.Layers.Overlaps(mask) && !visit(proxy)) {
									return false;
								}
							}
							return true;
						},
						keepWalking
					)) {
					return false;
				}
			}

			// After every populated level and in proxy order. A final residual is in
			// no level, so no early stop above may skip it.
			for (uint32_t index : grid.Oversized) {
				const Proxy &proxy = grid.Proxies[index];
				if (!proxy.Layers.Overlaps(mask)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}

		// Calls `visit` once for each proxy in a cell touched by a swept box.
		//
		// The centre follows the same differential walk as a ray. Each centre
		// cell opens a fixed neighbourhood large enough to contain the box at any
		// point in that cell, which makes the work linear in sweep length for a
		// fixed box rather than cubic in the swept bound.
		//
		// **De-duplication has two parts because this is a thick line.** A proxy
		// is reported from the first centre cell whose neighbourhood intersects
		// its cell range, then from the first shared cell inside that neighbourhood.
		// Expanding the proxy range by the neighbourhood radius makes the first
		// part the ray run rule over a different range. The second part is the
		// volume rule inside one neighbourhood. Together they are deterministic,
		// allocation-free, and leave the grid read-only.
		//
		// A walk estimated to open more cells than the grid holds takes the same
		// bounded scan fallback as the other walks. The caller still performs the
		// exact swept-box test, so conservative neighbourhood corners cost only a
		// rejected candidate.
		//
		// **The smaller traversal wins.** On the `bench` preset, 64 one-metre
		// boxes swept 64 metres diagonally through one-metre cells went from
		// 12.91 ms with the volume walk to 1.21 ms with this one. Forcing the same
		// walk on the existing short, axis-aligned row moved it from 42 ns to
		// 262 ns, so a swept envelope containing fewer cells keeps the volume
		// walk. This choice is geometry only and does not change the answer.
		//
		// @param reciprocal `RayReciprocal` for the centre line.
		// @param halfExtent  Half the swept box size on each axis.
		// @param sweptBounds The exact axis-aligned envelope, used by the scan fallback.
		// @param visit       Called with each candidate. Returning false stops the walk.
		// @return False if the visitor stopped the walk.
		template <class Visit>
		static bool ForEachCandidateAlongSweptBox(
			const HashGrid &grid,
			const core::Ray &ray,
			const RayReciprocal &reciprocal,
			float maxDistance,
			const core::Vector3 &halfExtent,
			const core::AABB &sweptBounds,
			LayerMask mask,
			Visit &&visit
		) {
			const float extent[3] = {halfExtent.X, halfExtent.Y, halfExtent.Z};
			const float origin[3] = {ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
			const float direction[3] = {ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
			auto estimateThickCells = [&](float inverseSpacing) {
				double neighbourhood = 1.0;
				for (int axis = 0; axis < 3; axis++) {
					const float radius = std::ceil(extent[axis] * inverseSpacing);
					if (!(radius >= 0.0f) || radius > static_cast<float>(CELL_LIMIT)) {
						return std::numeric_limits<double>::infinity();
					}
					neighbourhood *= static_cast<double>(radius) * 2.0 + 1.0;
				}
				double centre = 3.0;
				for (int axis = 0; axis < 3; axis++) {
					centre += static_cast<double>(std::abs(direction[axis])) *
								  static_cast<double>(maxDistance) * static_cast<double>(inverseSpacing) +
							  1.0;
				}
				return centre * neighbourhood;
			};
			auto volumeCells = [&](float inverseSpacing) {
				return CellsInRange(
					CellCoordinateOf(sweptBounds.Minimum.X, inverseSpacing),
					CellCoordinateOf(sweptBounds.Minimum.Y, inverseSpacing),
					CellCoordinateOf(sweptBounds.Minimum.Z, inverseSpacing),
					CellCoordinateOf(sweptBounds.Maximum.X, inverseSpacing),
					CellCoordinateOf(sweptBounds.Maximum.Y, inverseSpacing),
					CellCoordinateOf(sweptBounds.Maximum.Z, inverseSpacing)
				);
			};
			double aggregateVolume = 0.0;
			double aggregateThick = 0.0;
			auto addEstimate = [&](float inverseSpacing, const std::vector<HashGrid::Entry> &entries) {
				if (!entries.empty()) {
					aggregateVolume += static_cast<double>(volumeCells(inverseSpacing));
					aggregateThick += estimateThickCells(inverseSpacing);
				}
			};
			if (!grid.HasHierarchy) {
				// The ordinary scene keeps the original single-grid choice. The
				// hierarchy estimates and walks below are only for promoted storage.
				addEstimate(grid.InverseSpacing, grid.Entries);
			} else {
				addEstimate(grid.InverseSpacing, grid.Entries);
				for (const HashGrid::LevelStorage &storage : grid.CoarseLevels) {
					addEstimate(storage.InverseSpacing, storage.Entries);
				}
			}
			if (aggregateVolume <= aggregateThick) {
				return ForEachCandidate(grid, sweptBounds, mask, visit);
			}

			auto canWalk = [&](float inverseSpacing, const std::vector<HashGrid::Entry> &entries) {
				if (entries.empty()) {
					return true;
				}
				int32_t radius[3] = {0, 0, 0};
				double neighbourhoodCells = 1.0;
				for (int axis = 0; axis < 3; axis++) {
					const float cells = std::ceil(extent[axis] * inverseSpacing);
					if (!(cells >= 0.0f) || cells > static_cast<float>(CELL_LIMIT)) {
						return false;
					}
					radius[axis] = static_cast<int32_t>(cells);
					neighbourhoodCells *= static_cast<double>(radius[axis]) * 2.0 + 1.0;
				}
				const int32_t startCell[3] = {
					CellCoordinateOf(origin[0], inverseSpacing),
					CellCoordinateOf(origin[1], inverseSpacing),
					CellCoordinateOf(origin[2], inverseSpacing),
				};
				if (std::abs(startCell[0]) >= CELL_LIMIT || std::abs(startCell[1]) >= CELL_LIMIT ||
					std::abs(startCell[2]) >= CELL_LIMIT) {
					return false;
				}
				double centreCells = 3.0;
				for (int axis = 0; axis < 3; axis++) {
					centreCells += static_cast<double>(std::abs(direction[axis])) *
									   static_cast<double>(maxDistance) *
									   static_cast<double>(inverseSpacing) +
								   1.0;
				}
				return centreCells * neighbourhoodCells <=
					   static_cast<double>(entries.size()) + WALK_CELL_ALLOWANCE;
			};

			if (!canWalk(grid.InverseSpacing, grid.Entries)) {
				return ScanEveryProxy(grid, sweptBounds, mask, visit);
			}
			if (grid.HasHierarchy) {
				for (const HashGrid::LevelStorage &storage : grid.CoarseLevels) {
					if (!canWalk(storage.InverseSpacing, storage.Entries)) {
						return ScanEveryProxy(grid, sweptBounds, mask, visit);
					}
				}
			}

			auto walkLevel = [&](size_t levelIndex,
								 float spacing,
								 float inverseSpacing,
								 const std::vector<uint32_t> &bucketStart,
								 const std::vector<HashGrid::Entry> &entries) {
				if (entries.empty()) {
					return true;
				}
				int32_t radius[3] = {0, 0, 0};
				for (int axis = 0; axis < 3; axis++) {
					radius[axis] = static_cast<int32_t>(std::ceil(extent[axis] * inverseSpacing));
				}
				return ForEachCellAlongRay(
					spacing,
					inverseSpacing,
					ray,
					reciprocal,
					maxDistance,
					[&](const int32_t (&current)[3], const int32_t (&previous)[3], bool stepped) {
						const int32_t minimum[3] = {
							std::max(-CELL_LIMIT, current[0] - radius[0]),
							std::max(-CELL_LIMIT, current[1] - radius[1]),
							std::max(-CELL_LIMIT, current[2] - radius[2]),
						};
						const int32_t maximum[3] = {
							std::min(CELL_LIMIT, current[0] + radius[0]),
							std::min(CELL_LIMIT, current[1] + radius[1]),
							std::min(CELL_LIMIT, current[2] + radius[2]),
						};

						for (int32_t cellZ = minimum[2]; cellZ <= maximum[2]; cellZ++) {
							for (int32_t cellY = minimum[1]; cellY <= maximum[1]; cellY++) {
								for (int32_t cellX = minimum[0]; cellX <= maximum[0]; cellX++) {
									const size_t bucket =
										HashCell(cellX, cellY, cellZ) & (bucketStart.size() - 2);
									for (uint32_t slot = bucketStart[bucket]; slot < bucketStart[bucket + 1];
										 slot++) {
										const HashGrid::Entry &entry = entries[slot];
										if (entry.CellX != cellX || entry.CellY != cellY ||
											entry.CellZ != cellZ) {
											continue;
										}

										const HashGrid::CellRange &range = grid.Ranges[entry.ProxyIndex];
										if (range.Level != levelIndex ||
											cellX != std::max(range.MinimumX, minimum[0]) ||
											cellY != std::max(range.MinimumY, minimum[1]) ||
											cellZ != std::max(range.MinimumZ, minimum[2])) {
											continue;
										}
										if (stepped && WithinExpandedCellRange(range, previous, radius)) {
											continue;
										}

										const Proxy &proxy = grid.Proxies[entry.ProxyIndex];
										if (!proxy.Layers.Overlaps(mask)) {
											continue;
										}
										if (!visit(proxy)) {
											return false;
										}
									}
								}
							}
						}
						return true;
					},
					[](float) { return true; }
				);
			};

			if (!walkLevel(0, grid.Spacing, grid.InverseSpacing, grid.BucketStart, grid.Entries)) {
				return false;
			}
			if (grid.HasHierarchy) {
				for (size_t level = 1; level < HashGrid::HIERARCHY_LEVEL_COUNT; level++) {
					const HashGrid::LevelStorage &storage = grid.CoarseLevels[level - 1];
					if (!walkLevel(
							level,
							storage.Spacing,
							storage.InverseSpacing,
							storage.BucketStart,
							storage.Entries
						)) {
						return false;
					}
				}
			}

			for (uint32_t index : grid.Oversized) {
				const Proxy &proxy = grid.Proxies[index];
				if (!proxy.Layers.Overlaps(mask)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}

		// The box that encloses a ray segment. What the scan fallback is given,
		// and the volume the box walk used to be given for every raycast.
		static core::AABB SegmentBounds(const core::Ray &ray, float maxDistance) {
			const core::Vector3 end = ray.PointAt(maxDistance);
			return core::AABB{ray.Origin, ray.Origin}.Union(core::AABB{end, end});
		}

	  private:
		// How many cells are in an inclusive range, or zero when it is inverted.
		static int64_t CellsInRange(
			int32_t minimumX,
			int32_t minimumY,
			int32_t minimumZ,
			int32_t maximumX,
			int32_t maximumY,
			int32_t maximumZ
		) {
			if (maximumX < minimumX || maximumY < minimumY || maximumZ < minimumZ) {
				return 0;
			}
			return (static_cast<int64_t>(maximumX) - minimumX + 1) *
				   (static_cast<int64_t>(maximumY) - minimumY + 1) *
				   (static_cast<int64_t>(maximumZ) - minimumZ + 1);
		}

		// Walks the cells pierced by a ray and shares the DDA between the thin and
		// thick candidate walks. Candidate selection and de-duplication stay with
		// the walk whose shape defines them.
		template <class VisitCell, class KeepWalking>
		static bool ForEachCellAlongRay(
			float spacing,
			float inverseSpacing,
			const core::Ray &ray,
			const RayReciprocal &reciprocal,
			float maxDistance,
			VisitCell &&visitCell,
			KeepWalking &&keepWalking
		) {
			const float origin[3] = {ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
			const float direction[3] = {ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
			int32_t cell[3] = {
				CellCoordinateOf(origin[0], inverseSpacing),
				CellCoordinateOf(origin[1], inverseSpacing),
				CellCoordinateOf(origin[2], inverseSpacing),
			};
			int32_t step[3] = {0, 0, 0};
			float leaving[3] = {0.0f, 0.0f, 0.0f};
			float crossing[3] = {0.0f, 0.0f, 0.0f};

			for (int axis = 0; axis < 3; axis++) {
				if (reciprocal.Parallel[axis]) {
					step[axis] = 0;
					leaving[axis] = std::numeric_limits<float>::infinity();
					crossing[axis] = std::numeric_limits<float>::infinity();
					continue;
				}

				step[axis] = direction[axis] > 0.0f ? 1 : -1;
				const float plane =
					static_cast<float>(step[axis] > 0 ? cell[axis] + 1 : cell[axis]) * spacing;

				// Never behind the origin. A boundary can produce a small negative
				// through rounding, which would step before the query starts.
				leaving[axis] = std::max((plane - origin[axis]) * reciprocal.Inverse[axis], 0.0f);
				crossing[axis] = spacing * std::abs(reciprocal.Inverse[axis]);
			}

			bool stepped = false;
			int32_t previous[3] = {0, 0, 0};
			for (;;) {
				if (!visitCell(cell, previous, stepped)) {
					return false;
				}

				const float exit = std::min(leaving[0], std::min(leaving[1], leaving[2]));
				if (!keepWalking(exit) || !(exit < maxDistance)) {
					break;
				}

				int axis = 0;
				if (leaving[1] < leaving[axis]) {
					axis = 1;
				}
				if (leaving[2] < leaving[axis]) {
					axis = 2;
				}

				previous[0] = cell[0];
				previous[1] = cell[1];
				previous[2] = cell[2];
				stepped = true;

				cell[axis] += step[axis];
				if (cell[axis] < -CELL_LIMIT || cell[axis] > CELL_LIMIT) {
					break;
				}
				leaving[axis] += crossing[axis];
			}
			return true;
		}

		// Whether a cell coordinate lies inside a proxy's cell range.
		//
		// False for an oversized proxy, whose range is emptied at build time,
		// which is right: it is in no cell and the ray walk never reaches it
		// through one.
		static bool WithinCellRange(const HashGrid::CellRange &range, const int32_t (&cell)[3]) {
			return cell[0] >= range.MinimumX && cell[0] <= range.MaximumX && cell[1] >= range.MinimumY &&
				   cell[1] <= range.MaximumY && cell[2] >= range.MinimumZ && cell[2] <= range.MaximumZ;
		}

		// Whether a centre cell's neighbourhood intersects a proxy's range.
		static bool WithinExpandedCellRange(
			const HashGrid::CellRange &range, const int32_t (&cell)[3], const int32_t (&radius)[3]
		) {
			return cell[0] >= range.MinimumX - radius[0] && cell[0] <= range.MaximumX + radius[0] &&
				   cell[1] >= range.MinimumY - radius[1] && cell[1] <= range.MaximumY + radius[1] &&
				   cell[2] >= range.MinimumZ - radius[2] && cell[2] <= range.MaximumZ + radius[2];
		}

		// Every proxy, in order, with no cells involved. The answer for a query
		// so large that the walk would cost more than the scan.
		template <class Accept, class Visit>
		static bool ScanEveryProxyAccepted(
			const HashGrid &grid, const core::AABB &volume, LayerMask mask, Accept &&accept, Visit &&visit
		) {
			for (const Proxy &proxy : grid.Proxies) {
				if (!accept(proxy) || !proxy.Layers.Overlaps(mask) || !proxy.Bounds.Overlaps(volume)) {
					continue;
				}
				if (!visit(proxy)) {
					return false;
				}
			}
			return true;
		}

		template <class Visit>
		static bool
		ScanEveryProxy(const HashGrid &grid, const core::AABB &volume, LayerMask mask, Visit &&visit) {
			return ScanEveryProxyAccepted(
				grid, volume, mask, [](const Proxy &) { return true; }, std::forward<Visit>(visit)
			);
		}
	};
}
