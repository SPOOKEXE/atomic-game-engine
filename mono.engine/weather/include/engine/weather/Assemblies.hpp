#pragma once

// Camera-only eight-child summaries. Encoded sums conserve cosmetic codes within
// one profile; physical water uses the separately declared volume and dry-air weights.
//
// @tier L8 · shared

#include <engine/weather/Weather.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::weather {

	// Number of octree children represented by one camera-only summary.
	inline constexpr size_t ASSEMBLY_CHILDREN = 8;
	// Number of physical scalar channels summarized alongside child wind.
	inline constexpr size_t ASSEMBLY_SCALARS = 5;

	// One aligned parent and its eight children in the canonical lattice.
	struct AssemblyRegion {
		// Child zero origin; coordinates are ordered X bit 0, Y bit 1, Z bit 2.
		//@{
		CellCoordinate Origin;
		// Distance between child origins; parent origin aligns to twice this stride.
		uint32_t ChildStride = 1;
		// Number of coarsening levels above the canonical cell level.
		uint32_t Depth = 0;
		//@}
	};

	// Exact integer statistics for one encoded scalar across eight children.
	struct ScalarSummary {
		// Sum of the eight signed codes.
		//@{
		int32_t Sum = 0;
		// Population variance numerator; divide by 64 without rounding.
		uint32_t VarianceNumerator = 0;
		// Inclusive extrema among the child codes.
		int16_t Minimum = 0;
		int16_t Maximum = 0;
		//@}
	};

	// Camera-facing compact representation of a parent and its canonical children.
	struct alignas(16) AssemblySummary {
		// Region and accepted sample identity used to reject stale summaries.
		//@{
		AssemblyRegion Region;
		uint64_t Tick = 0;
		uint32_t Revision = 0;
		//@}
		// Condensate, temperature anomaly, vapour, pressure anomaly, ice.
		std::array<ScalarSummary, ASSEMBLY_SCALARS> Scalars{};
		// Rounded physical mean used when reconstructing the parent sample.
		CellValues Mean;
		// Separate values preserve divergence; averaging wind alone would erase it.
		std::array<std::array<int8_t, 3>, ASSEMBLY_CHILDREN> ChildWind{};
	};

	// Hysteresis bounds for compacting uniform regions and restoring their detail.
	struct AssemblyThresholds {
		// Maximum within-parent population variance and scalar spread by channel.
		//@{
		std::array<float, ASSEMBLY_SCALARS> MaximumVariance{4.0f, 4.0f, 4.0f, 4.0f, 4.0f};
		std::array<uint16_t, ASSEMBLY_SCALARS> MaximumContrast{8, 8, 8, 8, 8};
		//@}
		// Wind divergence limits in SI units; direction is ignored near zero speed.
		//@{
		float MaximumVelocityDifferenceMetresPerSecond = 2.0f;
		float MaximumSpeedDifferenceMetresPerSecond = 1.0f;
		float MaximumDirectionRadians = 0.15f;
		float NearZeroSpeedMetresPerSecond = 0.1f;
		//@}
		// Split threshold multiplier and minimum time a merge remains resident.
		//@{
		float SplitThresholdMultiplier = 1.5f;
		uint64_t MinimumResidenceTicks = 20;
		//@}
	};

	// Logical payload comparison between a dense child block and its summary.
	struct AssemblyStorage {
		// Dense encoded children, aligned summary, and separately retained winds.
		//@{
		size_t DenseBytes = ASSEMBLY_CHILDREN * ENCODED_CELL_BYTES;
		size_t SummaryBytes = sizeof(AssemblySummary);
		size_t RetainedWindBytes = ASSEMBLY_CHILDREN * 3;
		//@}
		// Dense payload divided by summary payload; excludes allocator overhead.
		float DenseToSummaryRatio = static_cast<float>(DenseBytes) / static_cast<float>(SummaryBytes);
	};

	// Per-child physical conversion weights for mass-conserving water totals.
	struct WaterWeights {
		// Child volume and dry-air density used by physical samples.
		//@{
		double VolumeCubicMetres = 1.0;
		double DryAirKilogramsPerCubicMetre = 1.225;
		//@}
	};

	// Conserved water totals and corresponding weighted mean concentrations.
	struct AssemblyWater {
		// Total species mass and weighted vapour/condensate mean values.
		//@{
		double VapourKilograms = 0.0;
		double CondensateKilograms = 0.0;
		double MeanVapourKilogramsPerKilogram = 0.0;
		double MeanCondensateKilogramsPerCubicMetre = 0.0;
		//@}
	};

	// Refuses unaligned regions, coordinate overflow, and reserved byte codes.
	[[nodiscard]] std::optional<std::array<CellCoordinate, ASSEMBLY_CHILDREN>>
	ChildCoordinates(const AssemblyRegion &region);
	// Builds exact encoded statistics and a deterministic physical mean for one completed sample.
	[[nodiscard]] std::optional<AssemblySummary> Summarize(
		const AssemblyRegion &region,
		const WeatherTime &time,
		std::span<const EncodedCell, ASSEMBLY_CHILDREN> children
	);
	// Procedural split always recovers the current source field, regardless of merge history.
	[[nodiscard]] std::optional<std::array<EncodedCell, ASSEMBLY_CHILDREN>> EvaluateChildren(
		const WeatherDefinition &definition, const WeatherTime &time, const AssemblyRegion &region
	);
	// Lossy split preserves each exact scalar sum and retained child winds, within stored bounds.
	// Seeds exclude allocation order, camera identity, and split count.
	[[nodiscard]] std::optional<std::array<EncodedCell, ASSEMBLY_CHILDREN>> SplitBalanced(
		const AssemblySummary &summary,
		uint32_t worldSeed,
		uint32_t layerKey,
		uint64_t timeEpoch,
		uint16_t maximumPerturbation = 4
	);
	// Allows compaction only when every scalar and wind channel fits its bound.
	[[nodiscard]] bool CanMerge(
		const AssemblySummary &summary,
		const EncodingProfile &profile,
		const AssemblyThresholds &thresholds = {}
	);
	// Applies relaxed split bounds after minimum residence, preventing camera-driven churn.
	[[nodiscard]] bool NeedsSplit(
		const AssemblySummary &summary,
		const EncodingProfile &profile,
		const AssemblyThresholds &thresholds,
		uint64_t mergedAtTick
	);
	// Unequal volumes and dry-air densities use physical weights, never integer code sums.
	[[nodiscard]] std::optional<AssemblyWater> WaterOf(
		std::span<const PhysicalSample, ASSEMBLY_CHILDREN> samples,
		std::span<const WaterWeights, ASSEMBLY_CHILDREN> weights
	);
}
