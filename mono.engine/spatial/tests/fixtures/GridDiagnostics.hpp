#pragma once

// Non-shipping diagnostics for cross-module test and benchmark fixtures. Spatial
// owns the private layout access and returns bounded values, never storage views.
#include "../../src/GridInternals.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::spatial::testing {
	struct GridLevelReading {
		size_t Proxies = 0;
		size_t Entries = 0;
		size_t OccupiedCells = 0;
		size_t MaximumCellMemberships = 0;
		size_t Buckets = 0;
		size_t OccupiedBuckets = 0;
		size_t MaximumBucketEntries = 0;
	};

	struct GridReading {
		size_t Proxies = 0;
		size_t ResidualProxies = 0;
		std::array<GridLevelReading, HashGrid::HIERARCHY_LEVEL_COUNT> Levels{};
	};

	inline GridReading ReadGrid(const HashGrid &grid, std::vector<std::array<int32_t, 3>> &scratch) {
		const auto internal = GridInternals::ReadOccupancy(grid, scratch);
		GridReading result;
		result.Proxies = internal.Proxies;
		result.ResidualProxies = internal.ResidualProxies;
		for (size_t level = 0; level < result.Levels.size(); level++) {
			const auto &value = internal.Levels[level];
			result.Levels[level] = {
				value.Proxies,
				value.Entries,
				value.OccupiedCells,
				value.MaximumCellMemberships,
				value.Buckets,
				value.OccupiedBuckets,
				value.MaximumBucketEntries
			};
		}
		return result;
	}
}
