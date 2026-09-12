#pragma once

// Frame-local ordering for the Asset Profiler. Kept beside the panel because
// it is a presentation rule, not a reusable engine data model.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace studio {
	enum class AssetProfileColumn : uint8_t {
		Asset,
		Kind,
		Pulled,
		Decoded,
		Cpu,
		Gpu,
		Updates,
		Resident,
		Delta,
		TotalResident,
	};

	// One requested order, most significant first.
	struct AssetProfileSort {
		AssetProfileColumn Column = AssetProfileColumn::Asset;
		bool Ascending = true;
	};

	// The sortable and rendered part of one row. It stays independent of
	// Editor's private content-pump state.
	struct AssetProfileSortRow {
		std::string_view Name;
		std::string_view Kind;
		uint64_t PulledBytes = 0;
		uint64_t DecodedBytes = 0;
		uint64_t CpuResidentBytes = 0;
		uint64_t GpuResidentBytes = 0;
		uint32_t Updates = 0;
		uint32_t ResidentInstances = 0;
		uint32_t StagedInstances = 0;
		uint64_t StagedBytes = 0;
		uint32_t Failures = 0;
	};

	// Orders frame-local rows. Equal requested values remain alphabetical so live
	// updates do not make the table shuffle needlessly.
	void SortAssetProfiles(std::vector<AssetProfileSortRow> &rows, std::span<const AssetProfileSort> sorts);
}
