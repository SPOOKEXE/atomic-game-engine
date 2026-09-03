#include "GridInternals.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <type_traits>

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

		// A promoted level only pays when its cells separate candidates. The
		// distributed promoted benchmark stays sparse at this limit, while the
		// identical-overlap benchmark otherwise builds a large table that every
		// query has to scan anyway.
		constexpr uint32_t MAXIMUM_COARSE_BUCKET_POPULATION = 256;

		// Logical shards come from the proxy count alone. A machine with more
		// workers receives the same cells in the same order as one that runs the
		// dispatcher inline; workers only change how quickly the fixed shards end.
		constexpr size_t PARALLEL_PROXY_GRAIN = 2048;

		// The stable radix workspace holds one temporary entry stream, two key
		// streams and 1024 counters per shard. Eight MiB bounds its retained
		// capacity; a larger scene stays on the exact serial representation.
		constexpr size_t MAXIMUM_PARALLEL_SCRATCH_BYTES = size_t{8} << 20;

		size_t ShardBegin(size_t shard, size_t shardCount, size_t proxyCount) {
			return proxyCount * shard / shardCount;
		}

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

	HashGridStats HashGrid::Stats() const {
		auto bytes = [](const auto &values, size_t live) {
			using Value = typename std::remove_reference_t<decltype(values)>::value_type;
			return HashGridStats{live * sizeof(Value), values.capacity() * sizeof(Value)};
		};
		auto add = [](HashGridStats &total, HashGridStats part) {
			total.LiveBytes += part.LiveBytes;
			total.RetainedBytes += part.RetainedBytes;
		};

		HashGridStats total{};
		add(total, bytes(Proxies, Proxies.size()));
		add(total, bytes(Ranges, Ranges.size()));
		add(total, bytes(BucketStart, BucketStart.size()));
		add(total, bytes(Entries, Entries.size()));
		add(total, bytes(Oversized, Oversized.size()));
		add(total, bytes(ParallelBucketScratch, ParallelBucketScratch.size()));
		add(total, bytes(ParallelEntryScratch, ParallelEntryScratch.size()));
		add(total, bytes(ParallelKeys, ParallelKeys.size()));
		add(total, bytes(ParallelKeyScratch, ParallelKeyScratch.size()));
		for (const LevelStorage &level : CoarseLevels) {
			add(total, bytes(level.BucketStart, level.BucketStart.size()));
			add(total, bytes(level.Entries, level.Entries.size()));
		}
		return total;
	}

	void HashGrid::Clear() {
		// Cleared, not freed. A grid rebuilt every tick over a steady scene
		// allocates on the first tick and never again.
		Proxies.clear();
		Ranges.clear();
		BucketStart.clear();
		Entries.clear();
		for (LevelStorage &level : CoarseLevels) {
			level.BucketStart.clear();
			level.Entries.clear();
		}
		Oversized.clear();
		ParallelBucketScratch.clear();
		ParallelEntryScratch.clear();
		ParallelKeys.clear();
		ParallelKeyScratch.clear();
		HasHierarchy = false;
	}

	void HashGrid::Rebuild(std::span<const Proxy> proxies) {
		Proxies.assign(proxies.begin(), proxies.end());
		BuildIndex();
	}

	void HashGrid::RebuildParallel(std::span<const Proxy> proxies, const RangeDispatcher &dispatcher) {
		Proxies.assign(proxies.begin(), proxies.end());
		BuildIndexParallel(dispatcher);
	}

	void HashGrid::BuildIndexParallel(const RangeDispatcher &dispatcher) {
		const auto scratchLiveBytes = [this]() {
			return ParallelBucketScratch.size() * sizeof(uint32_t) +
				   ParallelEntryScratch.size() * sizeof(Entry) + ParallelKeys.size() * sizeof(uint32_t) +
				   ParallelKeyScratch.size() * sizeof(uint32_t);
		};
		const auto scratchRetainedBytes = [this]() {
			return ParallelBucketScratch.capacity() * sizeof(uint32_t) +
				   ParallelEntryScratch.capacity() * sizeof(Entry) +
				   ParallelKeys.capacity() * sizeof(uint32_t) +
				   ParallelKeyScratch.capacity() * sizeof(uint32_t);
		};
		const auto publishSelection = [this,
									   &scratchLiveBytes,
									   &scratchRetainedBytes](size_t logicalShards, bool parallel) {
			core::Metrics::Count("spatial.grid.parallel.rebuilds", 1.0);
			core::Metrics::SetGauge(
				"spatial.grid.parallel.logical_shards", static_cast<double>(logicalShards)
			);
			core::Metrics::SetGauge("spatial.grid.parallel.used", parallel ? 1.0 : 0.0);
			core::Metrics::SetGauge(
				"spatial.grid.parallel.scratch_live_bytes", static_cast<double>(scratchLiveBytes())
			);
			core::Metrics::SetGauge(
				"spatial.grid.parallel.scratch_retained_bytes", static_cast<double>(scratchRetainedBytes())
			);
		};
		const auto serial = [&]() {
			ParallelBucketScratch.clear();
			ParallelEntryScratch.clear();
			ParallelKeys.clear();
			ParallelKeyScratch.clear();
			publishSelection(1, false);
			BuildIndex();
		};
		const auto releaseScratch = [this]() {
			std::vector<uint32_t>{}.swap(ParallelBucketScratch);
			std::vector<Entry>{}.swap(ParallelEntryScratch);
			std::vector<uint32_t>{}.swap(ParallelKeys);
			std::vector<uint32_t>{}.swap(ParallelKeyScratch);
		};

		if (dispatcher.Run == nullptr || Proxies.size() < PARALLEL_MINIMUM_PROXIES) {
			serial();
			return;
		}
		if (HasHierarchy) {
			// Repeated promoted scenes are the common hierarchy case. Their prior
			// layout proves the radix workspace cannot be used, so skip a second
			// classification pass before the exact serial hierarchy builder.
			core::Metrics::Count("spatial.grid.parallel.hierarchy_cached_serial_fallbacks", 1.0);
			serial();
			return;
		}

		const size_t candidateShards = std::min(
			MAXIMUM_PARALLEL_SHARDS, (Proxies.size() + PARALLEL_PROXY_GRAIN - 1) / PARALLEL_PROXY_GRAIN
		);
		if (candidateShards < 2) {
			serial();
			return;
		}

		const core::ScopedObservation timed("spatial.grid.rebuild.parallel");
		Ranges.clear();
		BucketStart.clear();
		Entries.clear();
		for (LevelStorage &level : CoarseLevels) {
			level.BucketStart.clear();
			level.Entries.clear();
		}
		Oversized.clear();
		HasHierarchy = false;
		Ranges.resize(Proxies.size());

		struct ClassifyContext {
			HashGrid *Grid;
			std::array<size_t, MAXIMUM_PARALLEL_SHARDS> *ShardEntries;
			std::array<uint8_t, MAXIMUM_PARALLEL_SHARDS> *BaseOnly;
			size_t Shards;

			static void Run(void *bodyContext, size_t beginShard, size_t endShard) {
				auto &context = *static_cast<ClassifyContext *>(bodyContext);
				for (size_t shard = beginShard; shard < endShard; shard++) {
					const size_t begin = ShardBegin(shard, context.Shards, context.Grid->Proxies.size());
					const size_t end = ShardBegin(shard + 1, context.Shards, context.Grid->Proxies.size());
					size_t entries = 0;
					bool baseOnly = true;
					for (size_t index = begin; index < end; index++) {
						const core::AABB &bounds = context.Grid->Proxies[index].Bounds;
						HashGrid::CellRange &range = context.Grid->Ranges[index];
						range.Level = 0;
						range.MinimumX = CellCoordinateOf(bounds.Minimum.X, context.Grid->InverseSpacing);
						range.MinimumY = CellCoordinateOf(bounds.Minimum.Y, context.Grid->InverseSpacing);
						range.MinimumZ = CellCoordinateOf(bounds.Minimum.Z, context.Grid->InverseSpacing);
						range.MaximumX = CellCoordinateOf(bounds.Maximum.X, context.Grid->InverseSpacing);
						range.MaximumY = CellCoordinateOf(bounds.Maximum.Y, context.Grid->InverseSpacing);
						range.MaximumZ = CellCoordinateOf(bounds.Maximum.Z, context.Grid->InverseSpacing);
						const int64_t spanX = static_cast<int64_t>(range.MaximumX) - range.MinimumX + 1;
						const int64_t spanY = static_cast<int64_t>(range.MaximumY) - range.MinimumY + 1;
						const int64_t spanZ = static_cast<int64_t>(range.MaximumZ) - range.MinimumZ + 1;
						const bool inverted = spanX <= 0 || spanY <= 0 || spanZ <= 0;
						const int64_t cells = inverted ? 0 : spanX * spanY * spanZ;
						if (inverted || cells > static_cast<int64_t>(HashGrid::MAXIMUM_CELLS_PER_PROXY)) {
							baseOnly = false;
							continue;
						}
						entries += static_cast<size_t>(cells);
					}
					(*context.ShardEntries)[shard] = entries;
					(*context.BaseOnly)[shard] = baseOnly ? 1 : 0;
				}
			}
		} classify{this, &ParallelShardEntries, &ParallelShardBaseOnly, candidateShards};
		{
			const core::ScopedObservation rangeTimed("spatial.grid.ranges.parallel");
			dispatcher.Run(dispatcher.Context, candidateShards, &ClassifyContext::Run, &classify);
		}
		core::Metrics::Count("spatial.grid.parallel.range_proxies", static_cast<double>(Proxies.size()));

		if (std::any_of(
				ParallelShardBaseOnly.begin(),
				ParallelShardBaseOnly.begin() + candidateShards,
				[](uint8_t value) { return value == 0; }
			)) {
			// The hierarchy is an exact extension of the serial builder. Falling
			// through here preserves its promotion and residual order until its own
			// multi-level count tables can share the same bounded scratch shape.
			core::Metrics::Count("spatial.grid.parallel.hierarchy_serial_fallbacks", 1.0);
			serial();
			return;
		}

		const size_t entryCount = std::accumulate(
			ParallelShardEntries.begin(), ParallelShardEntries.begin() + candidateShards, size_t{0}
		);
		const size_t buckets = ChooseBucketCount(entryCount);
		const size_t histogramBytes = candidateShards * 1024 * sizeof(uint32_t);
		const size_t entryScratchBytes = sizeof(Entry) + 2 * sizeof(uint32_t);
		const size_t maximumEntries =
			histogramBytes >= MAXIMUM_PARALLEL_SCRATCH_BYTES
				? 0
				: (MAXIMUM_PARALLEL_SCRATCH_BYTES - histogramBytes) / entryScratchBytes;
		const size_t logicalShards = entryCount <= maximumEntries ? candidateShards : 0;
		if (logicalShards < 2) {
			serial();
			return;
		}

		for (size_t shard = 0, offset = 0; shard < logicalShards; shard++) {
			ParallelShardEntryStarts[shard] = offset;
			offset += ParallelShardEntries[shard];
		}
		ParallelBucketScratch.assign(logicalShards * 1024, 0);
		Entries.resize(entryCount);
		ParallelEntryScratch.resize(entryCount);
		ParallelKeys.resize(entryCount);
		ParallelKeyScratch.resize(entryCount);
		if (scratchRetainedBytes() > MAXIMUM_PARALLEL_SCRATCH_BYTES) {
			releaseScratch();
			serial();
			return;
		}

		struct EnumerateContext {
			HashGrid *Grid;
			size_t Shards;
			size_t Buckets;

			static void Run(void *bodyContext, size_t beginShard, size_t endShard) {
				auto &context = *static_cast<EnumerateContext *>(bodyContext);
				for (size_t shard = beginShard; shard < endShard; shard++) {
					uint32_t *histogram = context.Grid->ParallelBucketScratch.data() + shard * 1024;
					const size_t begin = ShardBegin(shard, context.Shards, context.Grid->Proxies.size());
					const size_t end = ShardBegin(shard + 1, context.Shards, context.Grid->Proxies.size());
					size_t write = context.Grid->ParallelShardEntryStarts[shard];
					for (size_t index = begin; index < end; index++) {
						const HashGrid::CellRange &range = context.Grid->Ranges[index];
						ForEachCell(
							range.MinimumX,
							range.MinimumY,
							range.MinimumZ,
							range.MaximumX,
							range.MaximumY,
							range.MaximumZ,
							[&](int32_t cellX, int32_t cellY, int32_t cellZ) {
								context.Grid->Entries[write] = {
									cellX, cellY, cellZ, static_cast<uint32_t>(index)
								};
								const uint32_t key = HashCell(cellX, cellY, cellZ) & (context.Buckets - 1);
								context.Grid->ParallelKeys[write] = key;
								histogram[(key >> 10) & 1023]++;
								write++;
							}
						);
					}
				}
			}
		} enumerate{this, logicalShards, buckets};
		{
			const core::ScopedObservation enumerateTimed("spatial.grid.enumerate.parallel");
			dispatcher.Run(dispatcher.Context, logicalShards, &EnumerateContext::Run, &enumerate);
		}

		const auto prepareCursors = [this, logicalShards]() {
			std::array<uint32_t, 1025> starts{};
			for (size_t bin = 0; bin < 1024; bin++) {
				for (size_t shard = 0; shard < logicalShards; shard++) {
					starts[bin + 1] += this->ParallelBucketScratch[shard * 1024 + bin];
				}
				starts[bin + 1] += starts[bin];
			}
			for (size_t bin = 0; bin < 1024; bin++) {
				uint32_t cursor = starts[bin];
				for (size_t shard = 0; shard < logicalShards; shard++) {
					uint32_t &shardCursor = this->ParallelBucketScratch[shard * 1024 + bin];
					const uint32_t count = shardCursor;
					shardCursor = cursor;
					cursor += count;
				}
			}
		};
		prepareCursors();
		std::array<uint32_t, 1025> highStarts{};
		for (size_t bin = 1; bin < 1024; bin++) {
			highStarts[bin] = ParallelBucketScratch[bin];
		}
		highStarts[1024] = static_cast<uint32_t>(entryCount);
		struct HighScatterContext {
			HashGrid *Grid;
			size_t Shards;

			static void Run(void *bodyContext, size_t beginShard, size_t endShard) {
				auto &context = *static_cast<HighScatterContext *>(bodyContext);
				for (size_t shard = beginShard; shard < endShard; shard++) {
					uint32_t *cursors = context.Grid->ParallelBucketScratch.data() + shard * 1024;
					const size_t begin = context.Grid->ParallelShardEntryStarts[shard];
					const size_t end = begin + context.Grid->ParallelShardEntries[shard];
					for (size_t index = begin; index < end; index++) {
						const uint32_t key = context.Grid->ParallelKeys[index];
						const size_t output = cursors[(key >> 10) & 1023]++;
						context.Grid->ParallelEntryScratch[output] = context.Grid->Entries[index];
						context.Grid->ParallelKeyScratch[output] = key;
					}
				}
			}
		} highScatter{this, logicalShards};
		{
			const core::ScopedObservation highScatterTimed("spatial.grid.radix.high.scatter.parallel");
			dispatcher.Run(dispatcher.Context, logicalShards, &HighScatterContext::Run, &highScatter);
		}
		size_t highBinCount = 0;
		for (size_t bin = 0; bin < 1024; bin++) {
			if (highStarts[bin] != highStarts[bin + 1]) {
				ParallelHighBins[highBinCount++] = static_cast<uint16_t>(bin);
			}
		}
		BucketStart.assign(buckets + 1, 0);
		for (size_t high = 0; high < 1024; high++) {
			const size_t beginBucket = high * 1024;
			const size_t endBucket = std::min(buckets, beginBucket + 1024);
			for (size_t bucket = beginBucket; bucket < endBucket; bucket++) {
				BucketStart[bucket] = highStarts[high];
			}
		}
		BucketStart[buckets] = static_cast<uint32_t>(entryCount);
		struct LowSortContext {
			HashGrid *Grid;
			std::array<uint16_t, 1024> *HighBins;
			std::array<uint32_t, 1025> *HighStarts;
			size_t BinCount;
			size_t Buckets;

			static void Run(void *bodyContext, size_t beginBin, size_t endBin) {
				auto &context = *static_cast<LowSortContext *>(bodyContext);
				for (size_t binIndex = beginBin; binIndex < endBin; binIndex++) {
					const size_t high = (*context.HighBins)[binIndex];
					const size_t begin = (*context.HighStarts)[high];
					const size_t end = (*context.HighStarts)[high + 1];
					std::array<uint32_t, 1024> cursors{};
					for (size_t index = begin; index < end; index++) {
						const uint32_t key = context.Grid->ParallelKeyScratch[index];
						cursors[key & 1023]++;
					}
					uint32_t cursor = static_cast<uint32_t>(begin);
					for (size_t low = 0; low < 1024; low++) {
						const size_t bucket = high * 1024 + low;
						if (bucket < context.Buckets) {
							context.Grid->BucketStart[bucket] = cursor;
						}
						const uint32_t count = cursors[low];
						cursors[low] = cursor;
						cursor += count;
					}
					for (size_t index = begin; index < end; index++) {
						const uint32_t key = context.Grid->ParallelKeyScratch[index];
						const size_t output = cursors[key & 1023]++;
						context.Grid->Entries[output] = context.Grid->ParallelEntryScratch[index];
					}
				}
			}
		} lowSort{this, &ParallelHighBins, &highStarts, highBinCount, buckets};
		{
			const core::ScopedObservation lowSortTimed("spatial.grid.radix.low.sort.parallel");
			dispatcher.Run(dispatcher.Context, highBinCount, &LowSortContext::Run, &lowSort);
		}

		PublishedHierarchy = false;
		core::Metrics::Count("spatial.grid.proxies", static_cast<double>(Proxies.size()));
		core::Metrics::Count("spatial.grid.parallel.fill_entries", static_cast<double>(entryCount));
		publishSelection(logicalShards, true);
		ENGINE_TRACE(
			"rebuilt in parallel: {} proxies, {} cell entries, {} buckets, {} logical shards",
			Proxies.size(),
			entryCount,
			buckets,
			logicalShards
		);
	}

	void HashGrid::BuildIndex() {
		// **A histogram and not a counter.** This runs once a tick over the whole
		// scene, and what a stutter needs is the worst rebuild rather than the
		// mean of sixty of them.
		const core::ScopedObservation timed("spatial.grid.rebuild");
		ParallelBucketScratch.clear();
		ParallelEntryScratch.clear();
		ParallelKeys.clear();
		ParallelKeyScratch.clear();

		Ranges.clear();
		BucketStart.clear();
		Entries.clear();
		for (LevelStorage &level : CoarseLevels) {
			level.BucketStart.clear();
			level.Entries.clear();
		}
		Oversized.clear();
		Ranges.resize(Proxies.size());

		// Pass one: each proxy's cell range, and how many entries the whole set
		// needs. The ranges are kept rather than recomputed, because the two
		// passes below and every later query all want them, and six floors per
		// proxy repeated three times costs more than the bytes.
		std::array<size_t, HIERARCHY_LEVEL_COUNT> entryCounts{};
		std::array<size_t, HIERARCHY_LEVEL_COUNT> proxyCounts{};
		{
			const core::ScopedObservation rangeTimed("spatial.grid.ranges");
			for (size_t index = 0; index < Proxies.size(); index++) {
				const core::AABB &bounds = Proxies[index].Bounds;
				CellRange &range = Ranges[index];

				// The usual case is deliberately the old six-coordinate path. It
				// avoids validating every ordinary proxy and avoids entering the
				// hierarchy loop until the base placement actually fails.
				range.MinimumX = CellCoordinateOf(bounds.Minimum.X, InverseSpacing);
				range.MinimumY = CellCoordinateOf(bounds.Minimum.Y, InverseSpacing);
				range.MinimumZ = CellCoordinateOf(bounds.Minimum.Z, InverseSpacing);
				range.MaximumX = CellCoordinateOf(bounds.Maximum.X, InverseSpacing);
				range.MaximumY = CellCoordinateOf(bounds.Maximum.Y, InverseSpacing);
				range.MaximumZ = CellCoordinateOf(bounds.Maximum.Z, InverseSpacing);
				const int64_t spanX = static_cast<int64_t>(range.MaximumX) - range.MinimumX + 1;
				const int64_t spanY = static_cast<int64_t>(range.MaximumY) - range.MinimumY + 1;
				const int64_t spanZ = static_cast<int64_t>(range.MaximumZ) - range.MinimumZ + 1;
				const bool inverted = spanX <= 0 || spanY <= 0 || spanZ <= 0;
				const int64_t cells = inverted ? 0 : spanX * spanY * spanZ;
				if (!inverted && cells <= static_cast<int64_t>(MAXIMUM_CELLS_PER_PROXY)) {
					entryCounts[0] += static_cast<size_t>(cells);
					continue;
				}

				const bool valid = std::isfinite(bounds.Minimum.X) && std::isfinite(bounds.Minimum.Y) &&
								   std::isfinite(bounds.Minimum.Z) && std::isfinite(bounds.Maximum.X) &&
								   std::isfinite(bounds.Maximum.Y) && std::isfinite(bounds.Maximum.Z) &&
								   bounds.Minimum.X <= bounds.Maximum.X &&
								   bounds.Minimum.Y <= bounds.Maximum.Y &&
								   bounds.Minimum.Z <= bounds.Maximum.Z;
				bool placed = false;
				float levelInverse = InverseSpacing / HIERARCHY_SCALE;
				for (size_t level = 1; valid && level < HIERARCHY_LEVEL_COUNT; level++) {
					range.MinimumX = CellCoordinateOf(bounds.Minimum.X, levelInverse);
					range.MinimumY = CellCoordinateOf(bounds.Minimum.Y, levelInverse);
					range.MinimumZ = CellCoordinateOf(bounds.Minimum.Z, levelInverse);
					range.MaximumX = CellCoordinateOf(bounds.Maximum.X, levelInverse);
					range.MaximumY = CellCoordinateOf(bounds.Maximum.Y, levelInverse);
					range.MaximumZ = CellCoordinateOf(bounds.Maximum.Z, levelInverse);

					const uint64_t coarseSpanX =
						static_cast<uint64_t>(static_cast<int64_t>(range.MaximumX) - range.MinimumX + 1);
					const uint64_t coarseSpanY =
						static_cast<uint64_t>(static_cast<int64_t>(range.MaximumY) - range.MinimumY + 1);
					const uint64_t coarseSpanZ =
						static_cast<uint64_t>(static_cast<int64_t>(range.MaximumZ) - range.MinimumZ + 1);
					const bool fits =
						coarseSpanX != 0 && coarseSpanY != 0 && coarseSpanZ != 0 &&
						coarseSpanX <= MAXIMUM_CELLS_PER_PROMOTED_PROXY &&
						coarseSpanY <= MAXIMUM_CELLS_PER_PROMOTED_PROXY / coarseSpanX &&
						coarseSpanZ <= MAXIMUM_CELLS_PER_PROMOTED_PROXY / (coarseSpanX * coarseSpanY);
					if (!fits) {
						levelInverse /= HIERARCHY_SCALE;
						continue;
					}
					range.Level = static_cast<uint8_t>(level);
					entryCounts[level] += static_cast<size_t>(coarseSpanX * coarseSpanY * coarseSpanZ);
					proxyCounts[level]++;
					placed = true;
					break;
				}

				if (!placed) {
					Oversized.push_back(static_cast<uint32_t>(index));
					range.MaximumX = range.MinimumX - 1;
				}
			}
		}
		const bool hasPromotedCandidates =
			std::any_of(entryCounts.begin() + 1, entryCounts.end(), [](size_t count) { return count != 0; });
		if (!hasPromotedCandidates && Oversized.empty() && !PublishedHierarchy) {
			// Preserve the ordinary rebuild's original histogram and fill shape. A
			// grid that has never needed the hierarchy does not need its metrics or
			// retained-byte accounting on every tick.
			const size_t entryCount = entryCounts[0];
			const size_t buckets = ChooseBucketCount(entryCount);
			{
				const core::ScopedObservation histogramTimed("spatial.grid.histogram");
				BucketStart.assign(buckets + 1, 0);

				core::Metrics::Count("spatial.grid.proxies", static_cast<double>(Proxies.size()));

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
							Entries[BucketStart[bucket]++] =
								Entry{cellX, cellY, cellZ, static_cast<uint32_t>(index)};
						}
					);
				}
				for (size_t bucket = buckets; bucket > 0; bucket--) {
					BucketStart[bucket] = BucketStart[bucket - 1];
				}
				BucketStart[0] = 0;
			}

			HasHierarchy = false;
			return;
		}

		{
			const core::ScopedObservation histogramTimed("spatial.grid.histogram");
			if (hasPromotedCandidates) {
				float levelSpacing = Spacing * HIERARCHY_SCALE;
				for (LevelStorage &storage : CoarseLevels) {
					storage.Spacing = levelSpacing;
					storage.InverseSpacing = 1.0f / levelSpacing;
					levelSpacing *= HIERARCHY_SCALE;
				}
			}
			std::array<std::vector<uint32_t> *, HIERARCHY_LEVEL_COUNT> bucketStarts{&BucketStart};
			for (size_t level = 1; level < HIERARCHY_LEVEL_COUNT; level++) {
				bucketStarts[level] = &CoarseLevels[level - 1].BucketStart;
			}
			if (!hasPromotedCandidates) {
				BucketStart.assign(ChooseBucketCount(entryCounts[0]) + 1, 0);
			} else {
				for (size_t level = 0; level < HIERARCHY_LEVEL_COUNT; level++) {
					if (entryCounts[level] == 0) {
						continue;
					}
					const size_t buckets = ChooseBucketCount(entryCounts[level]);
					bucketStarts[level]->assign(buckets + 1, 0);
					if (buckets == MAXIMUM_BUCKET_COUNT && entryCounts[level] > buckets) {
						ENGINE_WARN_EVERY(
							10.0,
							"{} cell entries over the {} bucket cap at hierarchy level {}",
							entryCounts[level],
							MAXIMUM_BUCKET_COUNT,
							level
						);
					}
				}
			}
			for (const CellRange &range : Ranges) {
				if (range.MaximumX < range.MinimumX) {
					continue;
				}
				std::vector<uint32_t> &bucketStart = *bucketStarts[range.Level];
				const size_t buckets = bucketStart.size() - 1;
				ForEachCell(
					range.MinimumX,
					range.MinimumY,
					range.MinimumZ,
					range.MaximumX,
					range.MaximumY,
					range.MaximumZ,
					[&](int32_t cellX, int32_t cellY, int32_t cellZ) {
						bucketStart[(HashCell(cellX, cellY, cellZ) & (buckets - 1)) + 1]++;
					}
				);
			}
			if (hasPromotedCandidates) {
				std::array<bool, HIERARCHY_LEVEL_COUNT> rejected{};
				for (size_t level = 1; level < HIERARCHY_LEVEL_COUNT; level++) {
					std::vector<uint32_t> &bucketStart = *bucketStarts[level];
					if (bucketStart.empty()) {
						continue;
					}
					const uint32_t maximumPopulation =
						*std::max_element(bucketStart.begin() + 1, bucketStart.end());
					if (maximumPopulation <= MAXIMUM_COARSE_BUCKET_POPULATION) {
						continue;
					}

					rejected[level] = true;
					entryCounts[level] = 0;
					proxyCounts[level] = 0;
					bucketStart.clear();
				}
				if (std::any_of(rejected.begin(), rejected.end(), [](bool value) { return value; })) {
					// Rebuild this in source order. Appending rejected ranges after the
					// prior residual list would make fallback traversal history-dependent.
					Oversized.clear();
					for (size_t index = 0; index < Ranges.size(); index++) {
						CellRange &range = Ranges[index];
						if (range.MaximumX < range.MinimumX || rejected[range.Level]) {
							range.MaximumX = range.MinimumX - 1;
							Oversized.push_back(static_cast<uint32_t>(index));
						}
					}
				}
			}
			for (std::vector<uint32_t> *bucketStart : bucketStarts) {
				if (bucketStart->empty()) {
					continue;
				}
				for (size_t bucket = 0; bucket + 1 < bucketStart->size(); bucket++) {
					(*bucketStart)[bucket + 1] += (*bucketStart)[bucket];
				}
			}
		}
		const size_t promotedProxyCount =
			std::accumulate(proxyCounts.begin() + 1, proxyCounts.end(), size_t{0});
		proxyCounts[0] = Proxies.size() - Oversized.size() - promotedProxyCount;

		core::Metrics::Count("spatial.grid.proxies", static_cast<double>(Proxies.size()));
		HasHierarchy =
			!Oversized.empty() ||
			std::any_of(proxyCounts.begin() + 1, proxyCounts.end(), [](size_t count) { return count != 0; });
		const bool publishHierarchy = HasHierarchy || PublishedHierarchy;
		const char *const proxyMetrics[] = {
			"spatial.grid.level0.proxies",
			"spatial.grid.level1.proxies",
			"spatial.grid.level2.proxies",
			"spatial.grid.level3.proxies",
			"spatial.grid.level4.proxies",
		};
		const char *const entryMetrics[] = {
			"spatial.grid.level0.entries",
			"spatial.grid.level1.entries",
			"spatial.grid.level2.entries",
			"spatial.grid.level3.entries",
			"spatial.grid.level4.entries",
		};
		if (publishHierarchy) {
			core::Metrics::SetGauge(
				"spatial.grid.hierarchy.levels",
				static_cast<double>(std::count_if(proxyCounts.begin(), proxyCounts.end(), [](size_t count) {
					return count != 0;
				}))
			);
			for (size_t level = 0; level < HIERARCHY_LEVEL_COUNT; level++) {
				core::Metrics::SetGauge(proxyMetrics[level], static_cast<double>(proxyCounts[level]));
				core::Metrics::SetGauge(entryMetrics[level], static_cast<double>(entryCounts[level]));
			}
			core::Metrics::SetGauge("spatial.grid.oversized", static_cast<double>(Oversized.size()));
		}
		PublishedHierarchy = HasHierarchy;
		if (!Oversized.empty()) {
			ENGINE_DEBUG_EVERY(
				5.0,
				"{} of {} proxies use exact residual traversal after hierarchy placement",
				Oversized.size(),
				Proxies.size()
			);
		}

		{
			const core::ScopedObservation fillTimed("spatial.grid.fill");
			std::array<std::vector<uint32_t> *, HIERARCHY_LEVEL_COUNT> bucketStarts{&BucketStart};
			std::array<std::vector<Entry> *, HIERARCHY_LEVEL_COUNT> entries{&Entries};
			for (size_t level = 1; level < HIERARCHY_LEVEL_COUNT; level++) {
				bucketStarts[level] = &CoarseLevels[level - 1].BucketStart;
				entries[level] = &CoarseLevels[level - 1].Entries;
			}
			for (size_t level = 0; level < HIERARCHY_LEVEL_COUNT; level++) {
				if (entryCounts[level] != 0) {
					entries[level]->resize(entryCounts[level]);
				}
			}
			for (size_t index = 0; index < Ranges.size(); index++) {
				const CellRange &range = Ranges[index];
				if (range.MaximumX < range.MinimumX) {
					continue;
				}
				std::vector<uint32_t> &bucketStart = *bucketStarts[range.Level];
				std::vector<Entry> &levelEntries = *entries[range.Level];
				const size_t buckets = bucketStart.size() - 1;
				ForEachCell(
					range.MinimumX,
					range.MinimumY,
					range.MinimumZ,
					range.MaximumX,
					range.MaximumY,
					range.MaximumZ,
					[&](int32_t cellX, int32_t cellY, int32_t cellZ) {
						const size_t bucket = HashCell(cellX, cellY, cellZ) & (buckets - 1);
						levelEntries[bucketStart[bucket]++] =
							Entry{cellX, cellY, cellZ, static_cast<uint32_t>(index)};
					}
				);
			}
			for (std::vector<uint32_t> *bucketStart : bucketStarts) {
				if (bucketStart->empty()) {
					continue;
				}
				for (size_t bucket = bucketStart->size() - 1; bucket > 0; bucket--) {
					(*bucketStart)[bucket] = (*bucketStart)[bucket - 1];
				}
				(*bucketStart)[0] = 0;
			}

			ENGINE_TRACE(
				"rebuilt: {} proxies, {} base entries, {} hierarchy levels, {} residual",
				Proxies.size(),
				entryCounts[0],
				std::count_if(
					proxyCounts.begin(), proxyCounts.end(), [](size_t count) { return count != 0; }
				),
				Oversized.size()
			);
		}

		size_t retainedBytes = Proxies.capacity() * sizeof(Proxy) + Ranges.capacity() * sizeof(CellRange) +
							   BucketStart.capacity() * sizeof(uint32_t) +
							   Entries.capacity() * sizeof(Entry) + Oversized.capacity() * sizeof(uint32_t);
		for (const LevelStorage &level : CoarseLevels) {
			retainedBytes +=
				level.BucketStart.capacity() * sizeof(uint32_t) + level.Entries.capacity() * sizeof(Entry);
		}
		if (publishHierarchy) {
			core::Metrics::SetGauge(
				"spatial.grid.hierarchy.retained_bytes", static_cast<double>(retainedBytes)
			);
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
