#include "WeatherSample.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/weather/Weather.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::weather {

	namespace {

		bool Within(float value, float minimum, float maximum) {
			return std::isfinite(value) && value >= minimum && value <= maximum;
		}

		uint32_t UnsignedCode(float value, float scale) {
			return static_cast<uint32_t>(std::lround(value * 255.0f / scale));
		}

		int32_t SignedCode(float value, float scale) {
			return static_cast<int32_t>(std::lround(value * 127.0f / scale));
		}

		uint32_t Smooth(uint32_t fraction) {
			return (fraction * fraction * (768u - 2u * fraction) + 32768u) >> 16u;
		}

		uint32_t Interpolate(uint32_t first, uint32_t second, uint32_t fraction) {
			return (first * (256u - fraction) + second * fraction + 128u) >> 8u;
		}

		struct NoiseAxis {
			uint32_t Lattice = 0;
			uint32_t Fraction = 0;
			uint32_t Mask = 0;
		};

		NoiseAxis Axis(int32_t coordinate, uint32_t displacement, uint32_t shift) {
			const uint32_t fraction = displacement & 255u;
			// A 24-bit cell torus matches the Q8 displacement period, including negative wind.
			const uint32_t shifted =
				(static_cast<uint32_t>(coordinate) - (displacement >> 8u) - (fraction != 0 ? 1u : 0u)) &
				0x00ffffffu;
			const uint32_t remainder = fraction != 0 ? 256u - fraction : 0u;
			const uint32_t cellFraction = ((shifted & ((1u << shift) - 1u)) * 256u + remainder) >> shift;
			return {shifted >> shift, Smooth(cellFraction), 0x00ffffffu >> shift};
		}

		uint32_t Corner(uint32_t seed, uint32_t purpose, uint32_t epoch, uint32_t x, uint32_t y, uint32_t z) {
			return Hash(
					   seed ^ purpose ^ Hash(epoch) ^ (x * 0x9e3779b9u) ^ (y * 0x85ebca6bu) ^
					   (z * 0xc2b2ae35u)
				   ) >>
				   24u;
		}

		uint32_t
		NoiseAt(uint32_t seed, uint32_t purpose, uint32_t epoch, NoiseAxis x, NoiseAxis y, NoiseAxis z) {
			std::array<uint32_t, 4> rows{};
			for (uint32_t index = 0; index < 4; index++) {
				const uint32_t yCorner = (y.Lattice + (index & 1u)) & y.Mask;
				const uint32_t zCorner = (z.Lattice + (index >> 1u)) & z.Mask;
				rows[index] = Interpolate(
					Corner(seed, purpose, epoch, x.Lattice, yCorner, zCorner),
					Corner(seed, purpose, epoch, (x.Lattice + 1u) & x.Mask, yCorner, zCorner),
					x.Fraction
				);
			}
			return Interpolate(
				Interpolate(rows[0], rows[1], y.Fraction),
				Interpolate(rows[2], rows[3], y.Fraction),
				z.Fraction
			);
		}

		uint32_t Noise(
			const WeatherDefinition &definition,
			const WeatherTime &time,
			CellCoordinate coordinate,
			uint32_t purpose,
			uint32_t shift
		) {
			const NoiseAxis x = Axis(coordinate.X, time.DisplacementQ8[0], shift);
			const NoiseAxis y = Axis(coordinate.Y, time.DisplacementQ8[1], shift);
			const NoiseAxis z = Axis(coordinate.Z, time.DisplacementQ8[2], shift);
			const uint32_t seed = definition.Seed ^ Hash(definition.LayerKey);
			const uint32_t epoch = static_cast<uint32_t>(time.PhaseTick >> 8u);
			const uint32_t phase = Smooth(static_cast<uint32_t>(time.PhaseTick & 255u));
			return Interpolate(
				NoiseAt(seed, purpose, epoch, x, y, z), NoiseAt(seed, purpose, epoch + 1u, x, y, z), phase
			);
		}

	}

	uint32_t WeatherLayerKey(std::string_view layer) {
		uint32_t key = 2166136261u;
		for (const unsigned char character : layer) {
			key = (key ^ character) * 16777619u;
		}
		return Hash(key);
	}

	std::optional<CompiledWeather>
	Compile(const scene::WeatherCompute &controls, std::optional<uint32_t> layerKeyOverride) {
		if (!scene::ValidWeatherCompute(controls) || controls.Mode != scene::WeatherMode::Procedural ||
			controls.AlgorithmVersion != ALGORITHM_VERSION || !std::has_single_bit(controls.FeatureCells) ||
			controls.VapourMixingRatio > controls.VapourScale) {
			return std::nullopt;
		}
		CompiledWeather compiled;
		WeatherDefinition &definition = compiled.Definition;
		definition.Seed = controls.Seed;
		definition.LayerKey = layerKeyOverride.value_or(WeatherLayerKey(controls.LayerName.Text()));
		definition.Revision = controls.DefinitionRevision;
		definition.BaseCell =
			static_cast<int32_t>(std::floor(controls.BaseHeightMetres / controls.CellSizeMetres));
		definition.ThicknessCells = std::max(
			1u, static_cast<uint32_t>(std::ceil(controls.ThicknessMetres / controls.CellSizeMetres))
		);
		definition.FeatureShift = std::countr_zero(controls.FeatureCells);
		definition.Coverage = UnsignedCode(controls.Coverage, 1.0f);
		definition.Vapour = UnsignedCode(controls.VapourMixingRatio, controls.VapourScale);
		// This linear saturation response is a cosmetic approximation, not a thermodynamic solver.
		const float saturationPerCelsius = 0.0004f * 255.0f / controls.VapourScale;
		const float baseTemperature =
			controls.BaseTemperatureCelsius -
			controls.BaseHeightMetres * controls.TemperatureLapseCelsiusPerKilometre / 1000.0f;
		definition.SaturationBase = static_cast<int32_t>(std::lround(
			std::clamp((0.008f + baseTemperature * 0.0004f) * 255.0f / controls.VapourScale, 0.0f, 255.0f)
		));
		definition.SaturationLapseQ8 = static_cast<int32_t>(std::lround(
			std::clamp(
				saturationPerCelsius * controls.TemperatureLapseCelsiusPerKilometre *
					controls.CellSizeMetres / 1000.0f * 256.0f,
				-65536.0f,
				65536.0f
			)
		));
		definition.TemperatureSaturationQ8 = static_cast<uint32_t>(
			std::lround(std::min(saturationPerCelsius * 32.0f / 127.0f * 256.0f, 65536.0f))
		);
		definition.TemperatureVariation =
			static_cast<uint32_t>(SignedCode(controls.TemperatureVariationCelsius, 32.0f));
		definition.PressureVariation =
			static_cast<uint32_t>(SignedCode(controls.PressureVariationHectopascals, 32.0f));
		definition.Turbulence = static_cast<uint32_t>(SignedCode(controls.TurbulenceMetresPerSecond, 64.0f));
		definition.Shear = SignedCode(controls.ShearMetresPerSecond, 64.0f);
		definition.Lift = SignedCode(controls.LiftMetresPerSecond, 16.0f);
		definition.FrontContrast = UnsignedCode(controls.FrontContrast, 1.0f);
		definition.Growth = UnsignedCode(controls.GrowthRate, 1.0f);
		definition.Ice = UnsignedCode(controls.IceFraction, 1.0f);
		definition.WindCodes = {
			SignedCode(controls.WindMetresPerSecond.X, 64.0f),
			SignedCode(controls.WindMetresPerSecond.Y, 16.0f),
			SignedCode(controls.WindMetresPerSecond.Z, 64.0f)
		};
		const double displacementScale = 1048576.0 * static_cast<double>(controls.TimeScale) /
										 (static_cast<double>(controls.CellSizeMetres) * controls.TickRate);
		const std::array velocities{
			controls.WindMetresPerSecond.X, controls.WindMetresPerSecond.Y, controls.WindMetresPerSecond.Z
		};
		for (size_t axis = 0; axis < velocities.size(); axis++) {
			const int64_t quantized = std::llround(static_cast<double>(velocities[axis]) * displacementScale);
			if (quantized < -MAXIMUM_WIND_Q20 || quantized > MAXIMUM_WIND_Q20) {
				return std::nullopt;
			}
			definition.WindQ20[axis] = static_cast<int32_t>(quantized);
		}
		definition.TimeScaleQ16 = static_cast<uint32_t>(std::lround(controls.TimeScale * 65536.0f));
		definition.TickRate = controls.TickRate;
		definition.Paused = controls.Paused;
		EncodingProfile &profile = compiled.Profile;
		profile.CellSizeMetres = controls.CellSizeMetres;
		profile.CondensateKilogramsPerCubicMetre = controls.CondensateScale;
		profile.VapourKilogramsPerKilogram = controls.VapourScale;
		profile.BaseTemperatureCelsius = controls.BaseTemperatureCelsius;
		profile.BasePressureHectopascals = controls.BasePressureHectopascals;
		profile.TemperatureLapseCelsiusPerKilometre = controls.TemperatureLapseCelsiusPerKilometre;
		return compiled;
	}

	std::optional<EncodedCell> Encode(const CellValues &values) {
		if (values.TemperatureAnomaly < -127 || values.TemperatureAnomaly > 127 ||
			values.PressureAnomaly < -127 || values.PressureAnomaly > 127) {
			return std::nullopt;
		}
		for (int16_t wind : values.Wind) {
			if (wind < -127 || wind > 127) {
				return std::nullopt;
			}
		}
		return EncodedCell{
			{values.Condensate,
			 static_cast<uint8_t>(values.TemperatureAnomaly + 127),
			 values.Vapour,
			 static_cast<uint8_t>(values.PressureAnomaly + 127),
			 static_cast<uint8_t>(values.Wind[0] + 127),
			 static_cast<uint8_t>(values.Wind[1] + 127),
			 static_cast<uint8_t>(values.Wind[2] + 127),
			 values.Ice}
		};
	}

	std::optional<CellValues> Decode(const EncodedCell &cell) {
		for (uint32_t index : {1u, 3u, 4u, 5u, 6u}) {
			if (cell.Codes[index] == 255) {
				return std::nullopt;
			}
		}
		return CellValues{
			cell.Codes[0],
			static_cast<int16_t>(cell.Codes[1] - 127),
			cell.Codes[2],
			static_cast<int16_t>(cell.Codes[3] - 127),
			{static_cast<int16_t>(cell.Codes[4] - 127),
			 static_cast<int16_t>(cell.Codes[5] - 127),
			 static_cast<int16_t>(cell.Codes[6] - 127)},
			cell.Codes[7]
		};
	}

	uint32_t Hash(uint32_t value) {
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	}

	WeatherTimelineAnchor InitialAnchor(const WeatherDefinition &definition, uint64_t tick) {
		WeatherTimelineAnchor anchor;
		anchor.Tick = tick;
		anchor.WindQ20 = definition.WindQ20;
		anchor.TimeScaleQ16 = definition.TimeScaleQ16;
		anchor.Revision = definition.Revision;
		anchor.Paused = definition.Paused;
		return anchor;
	}

	std::optional<WeatherTime> ResolveTime(const WeatherTimelineAnchor &anchor, uint64_t tick) {
		if (tick < anchor.Tick || anchor.TimeScaleQ16 > 16u * 65536u || anchor.PhaseFractionQ16 > 65535u) {
			return std::nullopt;
		}
		for (size_t axis = 0; axis < anchor.WindQ20.size(); axis++) {
			if (anchor.WindQ20[axis] < -MAXIMUM_WIND_Q20 || anchor.WindQ20[axis] > MAXIMUM_WIND_Q20 ||
				anchor.DisplacementRemainderQ12[axis] >= (1u << DISPLACEMENT_REMAINDER_BITS)) {
				return std::nullopt;
			}
		}
		WeatherTime time;
		time.Tick = tick;
		time.PhaseTick = anchor.PhaseTick;
		time.DisplacementQ8 = anchor.DisplacementQ8;
		time.DisplacementRemainderQ12 = anchor.DisplacementRemainderQ12;
		time.PhaseFractionQ16 = anchor.PhaseFractionQ16;
		time.Revision = anchor.Revision;
		if (anchor.Paused) {
			return time;
		}
		const uint64_t elapsed = tick - anchor.Tick;
		// Split before multiplying so even UINT64_MAX seeking has defined modular uint64 phase.
		const uint64_t lowProduct = (elapsed & 65535u) * anchor.TimeScaleQ16 + anchor.PhaseFractionQ16;
		time.PhaseTick += (elapsed >> 16u) * anchor.TimeScaleQ16 + (lowProduct >> 16u);
		time.PhaseFractionQ16 = static_cast<uint32_t>(lowProduct & 65535u);
		constexpr uint64_t POSITION_MASK = (uint64_t{1} << (32u + DISPLACEMENT_REMAINDER_BITS)) - 1u;
		constexpr uint64_t REMAINDER_MASK = (uint64_t{1} << DISPLACEMENT_REMAINDER_BITS) - 1u;
		for (uint32_t axis = 0; axis < 3; axis++) {
			const uint64_t initialPosition =
				(static_cast<uint64_t>(anchor.DisplacementQ8[axis]) << DISPLACEMENT_REMAINDER_BITS) |
				anchor.DisplacementRemainderQ12[axis];
			// Modular uint64 multiplication preserves the required low 44 bits at every uint64 tick.
			// Sign extension precedes unsigned conversion so negative velocity borrows correctly.
			const uint64_t velocity = static_cast<uint64_t>(static_cast<int64_t>(anchor.WindQ20[axis]));
			const uint64_t position = (initialPosition + elapsed * velocity) & POSITION_MASK;
			time.DisplacementQ8[axis] = static_cast<uint32_t>(position >> DISPLACEMENT_REMAINDER_BITS);
			time.DisplacementRemainderQ12[axis] = static_cast<uint32_t>(position & REMAINDER_MASK);
		}
		return time;
	}

	std::optional<WeatherTimelineAnchor>
	Reanchor(const WeatherTimelineAnchor &anchor, const WeatherDefinition &definition, uint64_t tick) {
		const auto time = ResolveTime(anchor, tick);
		if (!time) {
			return std::nullopt;
		}
		WeatherTimelineAnchor result = InitialAnchor(definition, tick);
		result.PhaseTick = time->PhaseTick;
		result.PhaseFractionQ16 = time->PhaseFractionQ16;
		result.DisplacementQ8 = time->DisplacementQ8;
		result.DisplacementRemainderQ12 = time->DisplacementRemainderQ12;
		return result;
	}

	EncodedCell
	Evaluate(const WeatherDefinition &definition, const WeatherTime &time, CellCoordinate coordinate) {
		const uint32_t shift = std::min(definition.FeatureShift, 10u);
		const std::array<uint32_t, 8> noise{
			Noise(definition, time, coordinate, 0x46524f4eu, std::min(shift + 2u, 12u)),
			Noise(definition, time, coordinate, 0x4d4f4953u, shift),
			Noise(definition, time, coordinate, 0x424f4459u, shift),
			Noise(definition, time, coordinate, 0x45524f53u, shift > 2u ? shift - 2u : 0u),
			Noise(definition, time, coordinate, 0x54454d50u, shift),
			Noise(definition, time, coordinate, 0x57494e44u, shift),
			Noise(definition, time, coordinate, 0x57494e45u, shift),
			Noise(definition, time, coordinate, 0x57494e46u, shift)
		};
		return detail::FinishWeatherSample(definition, coordinate, noise);
	}

	std::optional<uint8_t>
	DensityUpperBound(const WeatherDefinition &definition, int32_t minimumY, int32_t maximumY) {
		if (minimumY > maximumY || definition.Version != ALGORITHM_VERSION || definition.Revision == 0 ||
			definition.ThicknessCells == 0 || definition.ThicknessCells > 65536 ||
			definition.FeatureShift > 10 || definition.Coverage > 255 || definition.Vapour > 255 ||
			definition.SaturationBase < 0 || definition.SaturationBase > 255 ||
			definition.SaturationLapseQ8 < -65536 || definition.SaturationLapseQ8 > 65536 ||
			definition.TemperatureSaturationQ8 > 65536 || definition.TemperatureVariation > 127 ||
			definition.PressureVariation > 127 || definition.Turbulence > 127 || definition.Shear < -127 ||
			definition.Shear > 127 || definition.Lift < -127 || definition.Lift > 127 ||
			definition.FrontContrast > 255 || definition.Growth > 255 || definition.Ice > 255 ||
			definition.TimeScaleQ16 > 16u * 65536u || definition.TickRate == 0 || definition.TickRate > 120) {
			return std::nullopt;
		}
		for (size_t axis = 0; axis < 3; axis++) {
			if (definition.WindCodes[axis] < -127 || definition.WindCodes[axis] > 127 ||
				definition.WindQ20[axis] < -MAXIMUM_WIND_Q20 || definition.WindQ20[axis] > MAXIMUM_WIND_Q20) {
				return std::nullopt;
			}
		}
		const int64_t lower =
			std::max(static_cast<int64_t>(minimumY), static_cast<int64_t>(definition.BaseCell));
		const int64_t upper = std::min(
			static_cast<int64_t>(maximumY),
			static_cast<int64_t>(definition.BaseCell) + definition.ThicknessCells - 1
		);
		if (definition.Coverage == 0 || lower > upper) return uint8_t{0};
		const int64_t minimumOffset = lower - definition.BaseCell;
		const int64_t maximumOffset = upper - definition.BaseCell;
		// The integer triangle is monotone on either side of its two central cells.
		// Clamp both centres into the interval so narrow pages retain a tight envelope bound.
		const std::array offsets{
			minimumOffset,
			maximumOffset,
			std::clamp(
				static_cast<int64_t>((definition.ThicknessCells - 1) / 2), minimumOffset, maximumOffset
			),
			std::clamp(static_cast<int64_t>(definition.ThicknessCells / 2), minimumOffset, maximumOffset)
		};
		uint32_t maximumEnvelope = 0;
		for (int64_t offset : offsets) {
			const uint32_t fraction = static_cast<uint32_t>(
				(offset * 2 + 1) * 255 / (static_cast<int64_t>(definition.ThicknessCells) * 2)
			);
			maximumEnvelope = std::max(maximumEnvelope, std::min(fraction * 2u, (255u - fraction) * 2u));
		}
		// Noise codes include 255, so their signed positive extreme is 128 rather than 127.
		const int32_t maximumFront = 128 * static_cast<int32_t>(definition.FrontContrast) / 255;
		const int32_t maximumVapour =
			std::min(255, static_cast<int32_t>(definition.Vapour) + 32 + maximumFront / 4);
		const int32_t minimumTemperature = detail::ClampSigned(
			(-127 - maximumFront / 2) * static_cast<int32_t>(definition.TemperatureVariation) / 127
		);
		const int64_t altitudeEndpoint = definition.SaturationLapseQ8 >= 0 ? maximumOffset : minimumOffset;
		// Wider multiplication is equivalent to the evaluator's split signed quotient/remainder.
		const int32_t maximumAltitudeResponse =
			static_cast<int32_t>(altitudeEndpoint * definition.SaturationLapseQ8 / 256);
		const int32_t minimumSaturation = std::clamp(
			definition.SaturationBase - maximumAltitudeResponse +
				minimumTemperature * static_cast<int32_t>(definition.TemperatureSaturationQ8) / 256,
			0,
			255
		);
		const uint32_t maximumExcess =
			maximumVapour > minimumSaturation ? static_cast<uint32_t>(maximumVapour - minimumSaturation) : 0u;
		// Coverage and growth each multiply by at most 255 and divide by 255, so cannot increase density.
		return static_cast<uint8_t>(maximumEnvelope * maximumExcess / 255u);
	}

	bool EvaluateBatch(
		const WeatherDefinition &definition,
		const WeatherTime &time,
		std::span<const CellCoordinate> coordinates,
		std::span<EncodedCell> output
	) {
		ENGINE_PROFILE("weather.evaluate");
		if (coordinates.size() != output.size()) {
			return false;
		}
		const size_t packedCount = detail::EvaluatePackedCells(definition, time, coordinates, output);
		for (size_t index = packedCount; index < coordinates.size(); index++) {
			output[index] = Evaluate(definition, time, coordinates[index]);
		}
		return true;
	}

	std::optional<PhysicalSample>
	ToPhysical(const EncodedCell &cell, const EncodingProfile &profile, float altitudeMetres) {
		const auto values = Decode(cell);
		if (!values || !Within(altitudeMetres, -1000000.0f, 1000000.0f) ||
			!Within(profile.CondensateKilogramsPerCubicMetre, 0.0f, 1.0f) ||
			!Within(profile.VapourKilogramsPerKilogram, 0.0f, 0.1f) ||
			!Within(profile.BaseTemperatureCelsius, -100.0f, 100.0f) ||
			!Within(profile.BasePressureHectopascals, 1.0f, 1200.0f) ||
			!Within(profile.TemperatureLapseCelsiusPerKilometre, -20.0f, 20.0f) ||
			!Within(profile.TemperatureAnomalyCelsius, 0.0f, 32.0f) ||
			!Within(profile.PressureAnomalyHectopascals, 0.0f, 32.0f) ||
			!Within(profile.WindScaleMetresPerSecond.X, 0.0f, 64.0f) ||
			!Within(profile.WindScaleMetresPerSecond.Y, 0.0f, 16.0f) ||
			!Within(profile.WindScaleMetresPerSecond.Z, 0.0f, 64.0f)) {
			return std::nullopt;
		}
		PhysicalSample sample;
		sample.CondensateKilogramsPerCubicMetre =
			values->Condensate * profile.CondensateKilogramsPerCubicMetre / 255.0f;
		sample.VapourKilogramsPerKilogram = values->Vapour * profile.VapourKilogramsPerKilogram / 255.0f;
		sample.TemperatureCelsius = profile.BaseTemperatureCelsius -
									altitudeMetres * profile.TemperatureLapseCelsiusPerKilometre / 1000.0f +
									values->TemperatureAnomaly * profile.TemperatureAnomalyCelsius / 127.0f;
		// The reference atmosphere is a declared linear cosmetic profile, bounded above vacuum.
		sample.PressureHectopascals = std::max(
			1.0f,
			profile.BasePressureHectopascals - altitudeMetres * 0.12f +
				values->PressureAnomaly * profile.PressureAnomalyHectopascals / 127.0f
		);
		sample.WindMetresPerSecond = {
			values->Wind[0] * profile.WindScaleMetresPerSecond.X / 127.0f,
			values->Wind[1] * profile.WindScaleMetresPerSecond.Y / 127.0f,
			values->Wind[2] * profile.WindScaleMetresPerSecond.Z / 127.0f
		};
		sample.IceFraction = values->Ice / 255.0f;
		return sample;
	}

	float
	ExtinctionPerMetre(float condensateKilogramsPerCubicMetre, float massExtinctionSquareMetresPerKilogram) {
		if (!std::isfinite(condensateKilogramsPerCubicMetre) ||
			!std::isfinite(massExtinctionSquareMetresPerKilogram) ||
			condensateKilogramsPerCubicMetre < 0.0f || massExtinctionSquareMetresPerKilogram < 0.0f) {
			return 0.0f;
		}
		const float extinction = condensateKilogramsPerCubicMetre * massExtinctionSquareMetresPerKilogram;
		return std::isfinite(extinction) ? extinction : 0.0f;
	}

	float WaterKilograms(
		const PhysicalSample &sample, float volumeCubicMetres, float dryAirKilogramsPerCubicMetre
	) {
		if (!std::isfinite(volumeCubicMetres) || volumeCubicMetres < 0.0f ||
			!std::isfinite(dryAirKilogramsPerCubicMetre) || dryAirKilogramsPerCubicMetre < 0.0f ||
			!Within(sample.CondensateKilogramsPerCubicMetre, 0.0f, 1.0f) ||
			!Within(sample.VapourKilogramsPerKilogram, 0.0f, 0.1f)) {
			return 0.0f;
		}
		const float water =
			volumeCubicMetres * (sample.CondensateKilogramsPerCubicMetre +
								 dryAirKilogramsPerCubicMetre * sample.VapourKilogramsPerKilogram);
		return std::isfinite(water) ? water : 0.0f;
	}
}
