#pragma once

// Shared scalar response and codec after either scalar or packed integer noise evaluation.

#include <engine/weather/Weather.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace engine::weather::detail {

	// How many bits to slow one noise purpose's evolution by. Each purpose
	// cross-fades between random lattices every `256 << shift` phase ticks.
	//
	// Real clouds keep their bodies for minutes and break up at the edges in
	// seconds, so the slow fields - fronts, the cloud body, moisture,
	// temperature and gusts - outlast the fast erosion detail. With one shared
	// rate the whole field re-randomised every 12.8 s at 20 Hz, and clouds
	// faded in and out in place instead of drifting and dissolving. Mirrored
	// by `WeatherEvolutionShift` in `weather-common.glsl`.
	inline uint32_t EvolutionShift(uint32_t purpose) {
		switch (purpose) {
		case 0x46524f4eu: return 6; // front
		case 0x4d4f4953u:			  // moisture
		case 0x424f4459u:			  // body
		case 0x54454d50u: return 4; // temperature
		case 0x45524f53u: return 0; // erosion detail
		default: return 3;			  // wind disturbance
		}
	}

	// The phase tick one purpose evolves on, after `EvolutionShift`.
	inline uint64_t EvolutionTick(uint64_t phaseTick, uint32_t purpose) {
		return phaseTick >> EvolutionShift(purpose);
	}

	// A fixed rotation of one purpose's horizontal noise lattice, as integer
	// cosine and sine scaled by a Pythagorean hypotenuse (5 or 13), plus the
	// bits added to its shift to undo most of that scale. The lattice reads
	// `u = x * Cos + z * Sin` and `v = z * Cos - x * Sin` in modular uint32, so
	// wind displacement rotates the same way and the 24-bit torus still holds.
	// Unrotated value noise sits on the cell axes and its features read as
	// axis-aligned boxes; the body and the erosion detail turn by different
	// angles so neither lattice lines up with the cells or with the other.
	// Mirrored by `WeatherRotation` in `weather-common.glsl`.
	struct LatticeRotation {
		uint32_t Cos = 0;
		uint32_t Sin = 0;
		uint32_t ExtraShift = 0;
	};
	inline LatticeRotation PurposeRotation(uint32_t purpose) {
		return purpose == 0x45524f53u ? LatticeRotation{12u, 5u, 4u} : LatticeRotation{3u, 4u, 2u};
	}

	inline int32_t ClampSigned(int32_t value) {
		return std::clamp(value, -127, 127);
	}
	inline uint8_t ClampByte(int32_t value) {
		return static_cast<uint8_t>(std::clamp(value, 0, 255));
	}
	inline uint32_t VerticalEnvelope(const WeatherDefinition &definition, int32_t y) {
		const int64_t offset = static_cast<int64_t>(y) - definition.BaseCell;
		if (offset < 0 || static_cast<uint64_t>(offset) >= definition.ThicknessCells) return 0;
		const uint32_t fraction =
			(static_cast<uint32_t>(offset) * 2u + 1u) * 255u / (definition.ThicknessCells * 2u);
		return std::min(fraction * 2u, (255u - fraction) * 2u);
	}

	inline EncodedCell FinishWeatherSample(
		const WeatherDefinition &definition, CellCoordinate coordinate, const std::array<uint32_t, 8> &noise
	) {
		const int32_t front = static_cast<int32_t>(noise[0]) - 127;
		const int32_t moisture = static_cast<int32_t>(noise[1]) - 127;
		const uint32_t body = noise[2];
		const uint32_t detail = noise[3];
		const int32_t temperature = static_cast<int32_t>(noise[4]) - 127;
		const uint32_t envelope = VerticalEnvelope(definition, coordinate.Y);
		const int32_t frontAmount = front * static_cast<int32_t>(definition.FrontContrast) / 255;
		const uint8_t vapour =
			ClampByte(static_cast<int32_t>(definition.Vapour) + moisture / 4 + frontAmount / 4);
		const uint32_t shape = static_cast<uint32_t>(
			std::clamp(static_cast<int32_t>((body * 3u + detail) / 4u) + frontAmount / 3, 0, 255)
		);
		const uint32_t threshold = 255u - std::min(definition.Coverage, 255u);
		const uint32_t covered =
			shape > threshold ? (shape - threshold) * 255u / std::max(definition.Coverage, 1u) : 0u;
		const uint32_t growth = 255u - definition.Growth + definition.Growth * body / 255u;
		const int32_t temperatureAnomaly = ClampSigned(
			(temperature - frontAmount / 2) * static_cast<int32_t>(definition.TemperatureVariation) / 127
		);
		// Clamp layer-local height before the bounded signed product, also outside the layer.
		const int32_t altitude = static_cast<int32_t>(
			std::clamp(static_cast<int64_t>(coordinate.Y) - definition.BaseCell, int64_t{0}, int64_t{65536})
		);
		const int32_t altitudeResponse = altitude * (definition.SaturationLapseQ8 / 256) +
										 altitude * (definition.SaturationLapseQ8 % 256) / 256;
		const int32_t saturation = std::clamp(
			definition.SaturationBase - altitudeResponse +
				temperatureAnomaly * static_cast<int32_t>(definition.TemperatureSaturationQ8) / 256,
			0,
			255
		);
		const uint32_t excess = vapour > saturation ? static_cast<uint32_t>(vapour - saturation) : 0u;
		const uint32_t condensate = covered * envelope / 255u * growth / 255u * excess / 255u;
		CellValues values;
		values.Condensate = static_cast<uint8_t>(condensate);
		values.Vapour = vapour;
		values.TemperatureAnomaly = static_cast<int16_t>(temperatureAnomaly);
		values.PressureAnomaly = static_cast<int16_t>(
			ClampSigned(front * static_cast<int32_t>(definition.PressureVariation) / 127)
		);
		for (uint32_t axis = 0; axis < 3; axis++) {
			const int32_t disturbance = static_cast<int32_t>(noise[5u + axis]) - 127;
			const int32_t altitudeShear =
				axis != 1u ? static_cast<int32_t>(envelope) * definition.Shear / 255 : 0;
			const int32_t lift = axis == 1u ? static_cast<int32_t>(condensate) * definition.Lift / 255 : 0;
			values.Wind[axis] = static_cast<int16_t>(ClampSigned(
				definition.WindCodes[axis] + disturbance * static_cast<int32_t>(definition.Turbulence) / 127 +
				altitudeShear + lift
			));
		}
		values.Ice = static_cast<uint8_t>(definition.Ice);
		return *Encode(values);
	}

	size_t EvaluatePackedCells(
		const WeatherDefinition &definition,
		const WeatherTime &time,
		std::span<const CellCoordinate> coordinates,
		std::span<EncodedCell> output
	);
}
