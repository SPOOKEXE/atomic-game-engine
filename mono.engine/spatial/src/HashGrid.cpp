#include "GridInternals.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::spatial {

	namespace {
		// The fewest buckets a grid ever has.
		//
		// Small enough that an almost-empty grid costs nothing to hold, large
		// enough that a handful of proxies do not all land in one bucket and
		// turn every query into a scan of the whole index.
		constexpr size_t MINIMUM_BUCKET_COUNT = 64;

		// The most it ever has. Four megabytes of bucket offsets, which is
		// where a wider table stops paying for itself against the cost of
		// touching it.
		constexpr size_t MAXIMUM_BUCKET_COUNT = size_t{1} << 20;

		// A power of two at least as large as the entry count.
		//
		// A power of two so that selecting a bucket is a mask rather than a
		// remainder. Not micro-optimisation for its own sake: selection happens
		// once per cell per query, and an integer division costs many times
		// what an AND does.
		size_t ChooseBucketCount(size_t entries) {
			size_t buckets = MINIMUM_BUCKET_COUNT;
			while (buckets < entries && buckets < MAXIMUM_BUCKET_COUNT) {
				buckets <<= 1;
			}
			return buckets;
		}

		// Every cell of an inclusive range, in the ascending order the query
		// walk uses.
		//
		// Both build passes go through here, so the counting pass and the
		// filling pass cannot disagree about which cells a proxy occupies -
		// which would corrupt the bucket offsets rather than merely misplace
		// something. A range whose maximum is below its minimum yields nothing,
		// and that is how an oversized proxy is kept out of the cells.
		template <class Visit>
		void ForEachCell(
			int32_t minimumX,
			int32_t minimumY,
			int32_t minimumZ,
			int32_t maximumX,
			int32_t maximumY,
			int32_t maximumZ,
			Visit &&visit
		) {
			for (int32_t cellZ = minimumZ; cellZ <= maximumZ; cellZ++) {
				for (int32_t cellY = minimumY; cellY <= maximumY; cellY++) {
					for (int32_t cellX = minimumX; cellX <= maximumX; cellX++) {
						visit(cellX, cellY, cellZ);
					}
				}
			}
		}
	}

	HashGrid::HashGrid(float cellSize) {
		// A zero or negative spacing makes the reciprocal an infinity or flips
		// every cell coordinate, and both reach every later query as a wrong
		// answer rather than as a failure. Falling back is louder than it
		// sounds: the grid keeps working and the caller's own cell-size
		// experiment stops changing anything, which is the symptom that gets
		// investigated.
		Spacing = cellSize > 0.0f ? cellSize : DEFAULT_CELL_SIZE;
		InverseSpacing = 1.0f / Spacing;

		// The symptom of the fallback is a cell-size experiment that stops
		// changing anything, so it says so rather than quietly working.
		if (!(cellSize > 0.0f)) {
			ENGINE_WARN("cell size {} is not positive; using the default {}", cellSize, DEFAULT_CELL_SIZE);
		}
	}

	void HashGrid::Clear() {
		// Cleared, not freed. A grid rebuilt every tick over a steady scene
		// allocates on the first tick and never again.
		Proxies.clear();
		Ranges.clear();
		BucketStart.clear();
		Entries.clear();
		Oversized.clear();
	}

	void HashGrid::Rebuild(std::span<const Proxy> proxies) {
		Proxies.assign(proxies.begin(), proxies.end());
		BuildIndex();
	}

	void HashGrid::BuildIndex() {
		// **A histogram and not a counter.** This runs once a tick over the whole
		// scene, and what a stutter needs is the worst rebuild rather than the
		// mean of sixty of them.
		const core::ScopedObservation timed("spatial.grid.rebuild");

		Ranges.clear();
		BucketStart.clear();
		Entries.clear();
		Oversized.clear();
		Ranges.resize(Proxies.size());

		// Pass one: each proxy's cell range, and how many entries the whole set
		// needs. The ranges are kept rather than recomputed, because the two
		// passes below and every later query all want them, and six floors per
		// proxy repeated three times costs more than the bytes.
		size_t entryCount = 0;
		{
			const core::ScopedObservation rangeTimed("spatial.grid.ranges");
			for (size_t index = 0; index < Proxies.size(); index++) {
				const core::AABB &bounds = Proxies[index].Bounds;
				CellRange &range = Ranges[index];

				range.MinimumX = CellCoordinateOf(bounds.Minimum.X, InverseSpacing);
				range.MinimumY = CellCoordinateOf(bounds.Minimum.Y, InverseSpacing);
				range.MinimumZ = CellCoordinateOf(bounds.Minimum.Z, InverseSpacing);
				range.MaximumX = CellCoordinateOf(bounds.Maximum.X, InverseSpacing);
				range.MaximumY = CellCoordinateOf(bounds.Maximum.Y, InverseSpacing);
				range.MaximumZ = CellCoordinateOf(bounds.Maximum.Z, InverseSpacing);

				const int64_t spanX = static_cast<int64_t>(range.MaximumX) - range.MinimumX + 1;
				const int64_t spanY = static_cast<int64_t>(range.MaximumY) - range.MinimumY + 1;
				const int64_t spanZ = static_cast<int64_t>(range.MaximumZ) - range.MinimumZ + 1;

				// A box built the wrong way round covers no cells and can still
				// satisfy AABB::Overlaps against a large enough box, so it joins
				// the oversized list where the exact test answers it - rather than
				// being dropped here, which would make the index disagree with the
				// type.
				const bool inverted = spanX <= 0 || spanY <= 0 || spanZ <= 0;
				const int64_t cells = inverted ? 0 : spanX * spanY * spanZ;

				if (inverted || cells > static_cast<int64_t>(MAXIMUM_CELLS_PER_PROXY)) {
					Oversized.push_back(static_cast<uint32_t>(index));

					// Emptied so that both build passes skip it without consulting
					// the list, and so that nothing later mistakes a huge range for
					// one somebody meant.
					range.MaximumX = range.MinimumX - 1;
					continue;
				}
				entryCount += static_cast<size_t>(cells);
			}
		}

		const size_t buckets = ChooseBucketCount(entryCount);
		{
			const core::ScopedObservation histogramTimed("spatial.grid.histogram");
			BucketStart.assign(buckets + 1, 0);

			core::Metrics::Count("spatial.grid.proxies", static_cast<double>(Proxies.size()));

			// **Every oversized proxy is tested exactly against every query**, so a
			// scene that accumulates them loses the acceleration one proxy at a
			// time with nothing reporting it.
			if (!Oversized.empty()) {
				core::Metrics::Count("spatial.grid.oversized", static_cast<double>(Oversized.size()));
				ENGINE_DEBUG_EVERY(
					5.0,
					"{} of {} proxies span more than {} cells at spacing {} and are tested exactly by every "
					"query",
					Oversized.size(),
					Proxies.size(),
					MAXIMUM_CELLS_PER_PROXY,
					Spacing
				);
			}

			// At the cap the table is no longer one bucket per entry, so a query
			// walks somebody else's cell contents on every lookup.
			if (buckets == MAXIMUM_BUCKET_COUNT && entryCount > buckets) {
				ENGINE_WARN_EVERY(
					10.0,
					"{} cell entries over the {} bucket cap; every query now scans a shared bucket",
					entryCount,
					MAXIMUM_BUCKET_COUNT
				);
			}

			ENGINE_TRACE(
				"rebuilt: {} proxies, {} cell entries, {} buckets, {} oversized",
				Proxies.size(),
				entryCount,
				buckets,
				Oversized.size()
			);

			// Pass two: how many entries each bucket owns, counted one slot to the
			// right so the prefix sum turns the counts into starts in place.
			for (const CellRange &range : Ranges) {
				ForEachCell(
					range.MinimumX,
					range.MinimumY,
					range.MinimumZ,
					range.MaximumX,
					range.MaximumY,
					range.MaximumZ,
					[&](int32_t cellX, int32_t cellY, int32_t cellZ) {
						BucketStart[(HashCell(cellX, cellY, cellZ) & (buckets - 1)) + 1]++;
					}
				);
			}
			for (size_t bucket = 0; bucket < buckets; bucket++) {
				BucketStart[bucket + 1] += BucketStart[bucket];
			}
		}

		{
			const core::ScopedObservation fillTimed("spatial.grid.fill");

			// Pass three: place. The starts serve as cursors while filling, then the
			// backward shift below restores them. Keeping a second bucket-sized array
			// for this one pass costs as much memory as the lookup table itself.
			Entries.resize(entryCount);
			for (size_t index = 0; index < Ranges.size(); index++) {
				const CellRange &range = Ranges[index];
				ForEachCell(
					range.MinimumX,
					range.MinimumY,
					range.MinimumZ,
					range.MaximumX,
					range.MaximumY,
					range.MaximumZ,
					[&](int32_t cellX, int32_t cellY, int32_t cellZ) {
						const size_t bucket = HashCell(cellX, cellY, cellZ) & (buckets - 1);
						Entries[BucketStart[bucket]++] = Entry{cellX, cellY, cellZ, static_cast<uint32_t>(index)};
					}
				);
			}

			// Every cursor now equals the old start of the bucket to its right. Shift
			// those ends back into place from right to left so no value is overwritten
			// before it is read. Entry order stays the proxy and cell order above.
			for (size_t bucket = buckets; bucket > 0; bucket--) {
				BucketStart[bucket] = BucketStart[bucket - 1];
			}
			BucketStart[0] = 0;
		}
	}

	void HashGrid::SetCellSize(float cellSize) {
		const float resolved = cellSize > 0.0f ? cellSize : DEFAULT_CELL_SIZE;
		if (resolved == Spacing) {
			// **Nothing dropped when nothing changed.** The caller asks every
			// time the set changes and the answer is usually the same one, so a
			// version that cleared unconditionally would throw away an index
			// that was about to be rebuilt identically.
			return;
		}

		ENGINE_DEBUG("cell size {} -> {}; the index is dropped", Spacing, resolved);

		Spacing = resolved;
		InverseSpacing = 1.0f / resolved;

		// Every entry records the cell a proxy was binned into, and those
		// coordinates are a function of the spacing. Keeping them would answer
		// queries against cells that no longer exist.
		Clear();
	}

	float SuggestCellSize(std::span<const Proxy> proxies) {
		if (proxies.empty()) {
			return HashGrid::DEFAULT_CELL_SIZE;
		}

		// **The mean of the widest axis, not of the volume.** A cell has to hold
		// a proxy along whichever direction it is longest, so a long thin wall
		// is a wall-sized object for this purpose even though its volume is
		// small.
		//
		// In `double`, because the sum is over every collider in the world and a
		// float accumulator loses the small ones once the running total is large
		// - which is the case a world of debris around one baseplate produces.
		double total = 0.0;
		for (const Proxy &proxy : proxies) {
			const core::Vector3 size = proxy.Bounds.Size();
			total += static_cast<double>(std::max({size.X, size.Y, size.Z}));
		}

		const auto mean = static_cast<float>(total / static_cast<double>(proxies.size()));

		// A world of degenerate boxes, or one whose bounds are not finite. The
		// default is the honest answer rather than a number derived from a NaN.
		if (!(mean > 0.0f)) {
			ENGINE_WARN(
				"{} proxies have a mean widest axis of {}; suggesting the default cell size instead",
				proxies.size(),
				mean
			);
			return HashGrid::DEFAULT_CELL_SIZE;
		}

		// Twice the mean is the rule of thumb `DEFAULT_CELL_SIZE` records, then
		// rounded to a power of two - see the header for why the rounding is the
		// hysteresis rather than a tidiness measure.
		const float wanted =
			std::clamp(mean * 2.0f, HashGrid::MINIMUM_CELL_SIZE, HashGrid::MAXIMUM_CELL_SIZE);

		// `exp2(round(log2(x)))` rather than a loop, so a world of buildings
		// costs the same as a world of pebbles.
		const float quantised = std::exp2(std::round(std::log2(wanted)));
		return std::clamp(quantised, HashGrid::MINIMUM_CELL_SIZE, HashGrid::MAXIMUM_CELL_SIZE);
	}
}
