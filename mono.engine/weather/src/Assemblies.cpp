#include <engine/weather/Assemblies.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::weather {
	namespace {
		std::array<int16_t, ASSEMBLY_SCALARS> ScalarsOf(const CellValues &cell) {
			return {cell.Condensate, cell.TemperatureAnomaly, cell.Vapour, cell.PressureAnomaly, cell.Ice};
		}

		void SetScalars(CellValues &cell, const std::array<int16_t, ASSEMBLY_SCALARS> &scalars) {
			cell.Condensate = static_cast<uint8_t>(scalars[0]);
			cell.TemperatureAnomaly = scalars[1];
			cell.Vapour = static_cast<uint8_t>(scalars[2]);
			cell.PressureAnomaly = scalars[3];
			cell.Ice = static_cast<uint8_t>(scalars[4]);
		}

		int32_t FloorEighth(int32_t sum) {
			return sum / 8 - (sum < 0 && sum % 8 != 0 ? 1 : 0);
		}

		bool ValidScalar(const ScalarSummary &scalar, size_t purpose) {
			const int32_t minimum = purpose == 1 || purpose == 3 ? -127 : 0;
			const int32_t maximum = purpose == 1 || purpose == 3 ? 127 : 255;
			return scalar.Minimum >= minimum && scalar.Maximum <= maximum &&
				   scalar.Minimum <= scalar.Maximum && scalar.Sum >= scalar.Minimum * 8 &&
				   scalar.Sum <= scalar.Maximum * 8;
		}

		uint32_t SplitSeed(
			const AssemblySummary &summary,
			uint32_t worldSeed,
			uint32_t layerKey,
			uint64_t timeEpoch,
			size_t purpose
		) {
			uint32_t seed = Hash(worldSeed ^ Hash(layerKey));
			seed = Hash(seed ^ static_cast<uint32_t>(summary.Region.Origin.X));
			seed = Hash(seed ^ static_cast<uint32_t>(summary.Region.Origin.Y));
			seed = Hash(seed ^ static_cast<uint32_t>(summary.Region.Origin.Z));
			seed = Hash(seed ^ summary.Region.ChildStride ^ Hash(summary.Region.Depth));
			seed =
				Hash(seed ^ static_cast<uint32_t>(timeEpoch) ^ Hash(static_cast<uint32_t>(timeEpoch >> 32)));
			return Hash(seed ^ static_cast<uint32_t>(purpose));
		}

		bool WithinThresholds(
			const AssemblySummary &summary,
			const EncodingProfile &profile,
			const AssemblyThresholds &thresholds,
			float multiplier
		) {
			if (!std::isfinite(multiplier) || multiplier < 1.0f) return false;
			for (size_t purpose = 0; purpose < ASSEMBLY_SCALARS; ++purpose) {
				const auto &scalar = summary.Scalars[purpose];
				const float variance = thresholds.MaximumVariance[purpose];
				if (!ValidScalar(scalar, purpose) || !std::isfinite(variance) || variance < 0.0f ||
					static_cast<float>(scalar.VarianceNumerator) > variance * 64.0f * multiplier ||
					static_cast<float>(scalar.Maximum - scalar.Minimum) >
						static_cast<float>(thresholds.MaximumContrast[purpose]) * multiplier)
					return false;
			}
			const float velocityLimit = thresholds.MaximumVelocityDifferenceMetresPerSecond * multiplier;
			const float speedLimit = thresholds.MaximumSpeedDifferenceMetresPerSecond * multiplier;
			const float directionLimit = thresholds.MaximumDirectionRadians * multiplier;
			const float nearZero = thresholds.NearZeroSpeedMetresPerSecond;
			if (!std::isfinite(velocityLimit) || velocityLimit < 0.0f || !std::isfinite(speedLimit) ||
				speedLimit < 0.0f || !std::isfinite(directionLimit) || directionLimit < 0.0f ||
				!std::isfinite(nearZero) || nearZero < 0.0f)
				return false;
			const auto scales = profile.WindScaleMetresPerSecond;
			if (!std::isfinite(scales.X) || !std::isfinite(scales.Y) || !std::isfinite(scales.Z) ||
				scales.X < 0.0f || scales.Y < 0.0f || scales.Z < 0.0f)
				return false;
			std::array<core::Vector3, ASSEMBLY_CHILDREN> winds{};
			std::array<float, ASSEMBLY_CHILDREN> speeds{};
			for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
				const auto &codes = summary.ChildWind[child];
				if (codes[0] == -128 || codes[1] == -128 || codes[2] == -128) return false;
				winds[child] =
					core::Vector3{
						static_cast<float>(codes[0]),
						static_cast<float>(codes[1]),
						static_cast<float>(codes[2])
					} *
					scales / 127.0f;
				speeds[child] = winds[child].Magnitude();
				if (!std::isfinite(speeds[child])) return false;
			}
			for (size_t first = 0; first < ASSEMBLY_CHILDREN; ++first) {
				for (size_t second = first + 1; second < ASSEMBLY_CHILDREN; ++second) {
					if ((winds[first] - winds[second]).Magnitude() > velocityLimit ||
						std::abs(speeds[first] - speeds[second]) > speedLimit)
						return false;
					const bool firstZero = speeds[first] <= nearZero;
					const bool secondZero = speeds[second] <= nearZero;
					if (firstZero != secondZero) return false;
					if (!firstZero &&
						(winds[first] / speeds[first]).Angle(winds[second] / speeds[second]) > directionLimit)
						return false;
				}
			}
			return true;
		}
	}

	std::optional<std::array<CellCoordinate, ASSEMBLY_CHILDREN>>
	ChildCoordinates(const AssemblyRegion &region) {
		const int64_t stride = region.ChildStride;
		if (!std::has_single_bit(region.ChildStride) || stride > std::numeric_limits<int32_t>::max())
			return std::nullopt;
		const std::array<int64_t, 3> origin{region.Origin.X, region.Origin.Y, region.Origin.Z};
		for (const int64_t axis : origin) {
			if (axis % (stride * 2) != 0 || axis + stride > std::numeric_limits<int32_t>::max())
				return std::nullopt;
		}
		std::array<CellCoordinate, ASSEMBLY_CHILDREN> coordinates{};
		for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
			coordinates[child] = {
				static_cast<int32_t>(origin[0] + ((child & 1) != 0 ? stride : 0)),
				static_cast<int32_t>(origin[1] + ((child & 2) != 0 ? stride : 0)),
				static_cast<int32_t>(origin[2] + ((child & 4) != 0 ? stride : 0))
			};
		}
		return coordinates;
	}

	std::optional<AssemblySummary> Summarize(
		const AssemblyRegion &region,
		const WeatherTime &time,
		std::span<const EncodedCell, ASSEMBLY_CHILDREN> children
	) {
		if (!ChildCoordinates(region)) return std::nullopt;
		AssemblySummary summary;
		summary.Region = region;
		summary.Tick = time.Tick;
		summary.Revision = time.Revision;
		std::array<uint32_t, ASSEMBLY_SCALARS> squareSums{};
		std::array<int32_t, 3> windSums{};
		for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
			const auto decoded = Decode(children[child]);
			if (!decoded) return std::nullopt;
			const auto scalars = ScalarsOf(*decoded);
			for (size_t purpose = 0; purpose < ASSEMBLY_SCALARS; ++purpose) {
				auto &scalar = summary.Scalars[purpose];
				scalar.Sum += scalars[purpose];
				squareSums[purpose] += static_cast<uint32_t>(scalars[purpose] * scalars[purpose]);
				scalar.Minimum = child == 0 ? scalars[purpose] : std::min(scalar.Minimum, scalars[purpose]);
				scalar.Maximum = child == 0 ? scalars[purpose] : std::max(scalar.Maximum, scalars[purpose]);
			}
			for (size_t axis = 0; axis < 3; ++axis) {
				summary.ChildWind[child][axis] = static_cast<int8_t>(decoded->Wind[axis]);
				windSums[axis] += decoded->Wind[axis];
			}
		}
		std::array<int16_t, ASSEMBLY_SCALARS> means{};
		for (size_t purpose = 0; purpose < ASSEMBLY_SCALARS; ++purpose) {
			auto &scalar = summary.Scalars[purpose];
			scalar.VarianceNumerator =
				squareSums[purpose] * 8 - static_cast<uint32_t>(scalar.Sum * scalar.Sum);
			means[purpose] = static_cast<int16_t>(FloorEighth(scalar.Sum));
		}
		SetScalars(summary.Mean, means);
		for (size_t axis = 0; axis < 3; ++axis)
			summary.Mean.Wind[axis] = static_cast<int16_t>(windSums[axis] / 8);
		return summary;
	}

	std::optional<std::array<EncodedCell, ASSEMBLY_CHILDREN>> EvaluateChildren(
		const WeatherDefinition &definition, const WeatherTime &time, const AssemblyRegion &region
	) {
		const auto coordinates = ChildCoordinates(region);
		if (!coordinates || time.Revision != definition.Revision) return std::nullopt;
		std::array<EncodedCell, ASSEMBLY_CHILDREN> children{};
		if (!EvaluateBatch(definition, time, *coordinates, children)) return std::nullopt;
		return children;
	}

	std::optional<std::array<EncodedCell, ASSEMBLY_CHILDREN>> SplitBalanced(
		const AssemblySummary &summary,
		uint32_t worldSeed,
		uint32_t layerKey,
		uint64_t timeEpoch,
		uint16_t maximumPerturbation
	) {
		if (!ChildCoordinates(summary.Region)) return std::nullopt;
		std::array<CellValues, ASSEMBLY_CHILDREN> values{};
		std::array<std::array<int16_t, ASSEMBLY_SCALARS>, ASSEMBLY_CHILDREN> scalars{};
		for (size_t purpose = 0; purpose < ASSEMBLY_SCALARS; ++purpose) {
			const auto &scalar = summary.Scalars[purpose];
			if (!ValidScalar(scalar, purpose)) return std::nullopt;
			uint32_t seed = SplitSeed(summary, worldSeed, layerKey, timeEpoch, purpose);
			const int32_t quotient = FloorEighth(scalar.Sum);
			const uint32_t remainder = static_cast<uint32_t>(scalar.Sum - quotient * 8);
			const size_t rotation = seed & 7;
			for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
				scalars[(child + rotation) & 7][purpose] =
					static_cast<int16_t>(quotient + (child < remainder ? 1 : 0));
			}
			for (size_t pair = 0; pair < ASSEMBLY_CHILDREN / 2; ++pair) {
				seed = Hash(seed);
				const size_t first = (pair * 2 + rotation) & 7;
				const size_t second = (pair * 2 + 1 + rotation) & 7;
				const bool reverse = (seed & 1) != 0;
				auto &increased = scalars[reverse ? second : first][purpose];
				auto &decreased = scalars[reverse ? first : second][purpose];
				const int32_t allowance = std::min(
					{static_cast<int32_t>(maximumPerturbation),
					 static_cast<int32_t>(scalar.Maximum - increased),
					 static_cast<int32_t>(decreased - scalar.Minimum)}
				);
				const int16_t perturbation =
					static_cast<int16_t>((seed >> 1) % (static_cast<uint32_t>(allowance) + 1));
				increased = static_cast<int16_t>(increased + perturbation);
				decreased = static_cast<int16_t>(decreased - perturbation);
			}
		}
		std::array<EncodedCell, ASSEMBLY_CHILDREN> children{};
		for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
			SetScalars(values[child], scalars[child]);
			for (size_t axis = 0; axis < 3; ++axis)
				values[child].Wind[axis] = summary.ChildWind[child][axis];
			const auto encoded = Encode(values[child]);
			if (!encoded) return std::nullopt;
			children[child] = *encoded;
		}
		return children;
	}

	bool CanMerge(
		const AssemblySummary &summary, const EncodingProfile &profile, const AssemblyThresholds &thresholds
	) {
		return ChildCoordinates(summary.Region).has_value() &&
			   WithinThresholds(summary, profile, thresholds, 1.0f);
	}

	bool NeedsSplit(
		const AssemblySummary &summary,
		const EncodingProfile &profile,
		const AssemblyThresholds &thresholds,
		uint64_t mergedAtTick
	) {
		if (summary.Tick < mergedAtTick || summary.Tick - mergedAtTick < thresholds.MinimumResidenceTicks)
			return false;
		return !WithinThresholds(summary, profile, thresholds, thresholds.SplitThresholdMultiplier);
	}

	std::optional<AssemblyWater> WaterOf(
		std::span<const PhysicalSample, ASSEMBLY_CHILDREN> samples,
		std::span<const WaterWeights, ASSEMBLY_CHILDREN> weights
	) {
		AssemblyWater water;
		double totalVolume = 0.0;
		double totalDryAir = 0.0;
		for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
			const auto &sample = samples[child];
			const auto &weight = weights[child];
			if (!std::isfinite(weight.VolumeCubicMetres) || weight.VolumeCubicMetres <= 0.0 ||
				!std::isfinite(weight.DryAirKilogramsPerCubicMetre) ||
				weight.DryAirKilogramsPerCubicMetre <= 0.0 ||
				!std::isfinite(sample.VapourKilogramsPerKilogram) ||
				sample.VapourKilogramsPerKilogram < 0.0f ||
				!std::isfinite(sample.CondensateKilogramsPerCubicMetre) ||
				sample.CondensateKilogramsPerCubicMetre < 0.0f)
				return std::nullopt;
			const double dryAirMass = weight.VolumeCubicMetres * weight.DryAirKilogramsPerCubicMetre;
			if (!std::isfinite(dryAirMass) || dryAirMass <= 0.0) return std::nullopt;
			water.VapourKilograms += dryAirMass * sample.VapourKilogramsPerKilogram;
			water.CondensateKilograms += weight.VolumeCubicMetres * sample.CondensateKilogramsPerCubicMetre;
			totalVolume += weight.VolumeCubicMetres;
			totalDryAir += dryAirMass;
		}
		if (!std::isfinite(totalVolume) || !std::isfinite(totalDryAir) ||
			!std::isfinite(water.VapourKilograms) || !std::isfinite(water.CondensateKilograms))
			return std::nullopt;
		water.MeanVapourKilogramsPerKilogram = water.VapourKilograms / totalDryAir;
		water.MeanCondensateKilogramsPerCubicMetre = water.CondensateKilograms / totalVolume;
		return water;
	}
}
