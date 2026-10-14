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

	inline constexpr size_t ASSEMBLY_CHILDREN = 8;
	inline constexpr size_t ASSEMBLY_SCALARS = 5;

	// Child order is X bit 0, Y bit 1, Z bit 2. Origin aligns to twice ChildStride.
	struct AssemblyRegion {
		CellCoordinate Origin;
		uint32_t ChildStride = 1;
		uint32_t Depth = 0;
	};

	// VarianceNumerator / 64 is the population variance of eight codes, without rounding.
	struct ScalarSummary {
		int32_t Sum = 0;
		uint32_t VarianceNumerator = 0;
		int16_t Minimum = 0;
		int16_t Maximum = 0;
	};

	struct alignas(16) AssemblySummary {
		AssemblyRegion Region;
		uint64_t Tick = 0;
		uint32_t Revision = 0;
		// Condensate, temperature anomaly, vapour, pressure anomaly, ice.
		std::array<ScalarSummary, ASSEMBLY_SCALARS> Scalars{};
		CellValues Mean;
		// Separate values preserve divergence; averaging wind alone would erase it.
		std::array<std::array<int8_t, 3>, ASSEMBLY_CHILDREN> ChildWind{};
	};

	struct AssemblyThresholds {
		std::array<float, ASSEMBLY_SCALARS> MaximumVariance{4.0f, 4.0f, 4.0f, 4.0f, 4.0f};
		std::array<uint16_t, ASSEMBLY_SCALARS> MaximumContrast{8, 8, 8, 8, 8};
		float MaximumVelocityDifferenceMetresPerSecond = 2.0f;
		float MaximumSpeedDifferenceMetresPerSecond = 1.0f;
		float MaximumDirectionRadians = 0.15f;
		float NearZeroSpeedMetresPerSecond = 0.1f;
		float SplitThresholdMultiplier = 1.5f;
		uint64_t MinimumResidenceTicks = 20;
	};

	struct AssemblyStorage {
		size_t DenseBytes = ASSEMBLY_CHILDREN * ENCODED_CELL_BYTES;
		size_t SummaryBytes = sizeof(AssemblySummary);
		size_t RetainedWindBytes = ASSEMBLY_CHILDREN * 3;
		float DenseToSummaryRatio = static_cast<float>(DenseBytes) / static_cast<float>(SummaryBytes);
	};

	struct WaterWeights {
		double VolumeCubicMetres = 1.0;
		double DryAirKilogramsPerCubicMetre = 1.225;
	};

	struct AssemblyWater {
		double VapourKilograms = 0.0;
		double CondensateKilograms = 0.0;
		double MeanVapourKilogramsPerKilogram = 0.0;
		double MeanCondensateKilogramsPerCubicMetre = 0.0;
	};

	// Refuses unaligned regions, coordinate overflow, and reserved byte codes.
	[[nodiscard]] std::optional<std::array<CellCoordinate, ASSEMBLY_CHILDREN>>
	ChildCoordinates(const AssemblyRegion &region);
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
	[[nodiscard]] bool CanMerge(
		const AssemblySummary &summary,
		const EncodingProfile &profile,
		const AssemblyThresholds &thresholds = {}
	);
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
