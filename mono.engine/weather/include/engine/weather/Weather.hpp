#pragma once

// Versioned integer reference weather. This evaluates queries, not a mirrored resident volume.
// Quantized inputs and the byte codec are portable semantic values, not a device buffer ABI.
//
// @tier L8 · shared

#include <engine/scene/WeatherCompute.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace engine::weather {

	// Fixed overflow/interpolation contract implemented by CPU and shader references.
	inline constexpr uint32_t ALGORITHM_VERSION = 1;
	// Every reference sample occupies eight semantic byte codes, including its ice fraction.
	inline constexpr uint32_t ENCODED_CELL_BYTES = 8;
	// Greatest supported Q20 per-tick velocity, derived from semantic wind/rate/scale bounds.
	inline constexpr int32_t MAXIMUM_WIND_Q20 = 1073741824;
	// Fractional bits retained below the Q8 shader sampling position.
	inline constexpr uint32_t DISPLACEMENT_REMAINDER_BITS = 12;

	// Signed canonical world-lattice coordinate; camera rebasing never changes it.
	// Reference noise is periodic every 2^24 cells per axis to match Q8 displacement wrapping.
	struct CellCoordinate {
		// Signed canonical X cell coordinate.
		int32_t X = 0;
		// Signed canonical Y cell coordinate.
		int32_t Y = 0;
		// Signed canonical Z cell coordinate.
		int32_t Z = 0;
		// Compares semantic values without padding or device layout.
		bool operator==(const CellCoordinate &) const = default;
	};

	// Portable byte-code sample. Signed anomalies use biased [-127,127] codes; 255 is reserved.
	// Layout is semantic codec order, independent of device word layout.
	struct EncodedCell {
		// Density, temperature, vapour, pressure, wind X/Y/Z, then ice fraction.
		std::array<uint8_t, ENCODED_CELL_BYTES> Codes{};
		// Compares semantic values without padding or device layout.
		bool operator==(const EncodedCell &) const = default;
	};

	// Unpacked normalized codes with explicit meanings, independent of host byte order.
	struct CellValues {
		// Unsigned condensate concentration code.
		uint8_t Condensate = 0;
		// Signed temperature departure from the reference profile.
		int16_t TemperatureAnomaly = 0;
		// Unsigned vapour mixing-ratio code.
		uint8_t Vapour = 0;
		// Signed pressure departure from the reference profile.
		int16_t PressureAnomaly = 0;
		// Signed velocity codes in X/Y/Z order.
		std::array<int16_t, 3> Wind{};
		// Unsigned ice fraction code.
		uint8_t Ice = 0;
		// Compares semantic values without padding or device layout.
		bool operator==(const CellValues &) const = default;
	};

	// Canonical quantization prepared once at the semantic adapter boundary.
	struct WeatherDefinition {
		// Version of integer evaluation and rounding rules.
		uint32_t Version = ALGORITHM_VERSION;
		// Authored world/layer seed.
		uint32_t Seed = 1;
		// Stable semantic layer key, never an entity or allocation index.
		uint32_t LayerKey = 0;
		// Accepted definition/timeline revision for cache identity.
		uint32_t Revision = 1;
		// Bottom canonical Y cell of the finite layer.
		int32_t BaseCell = 0;
		// Positive layer height in canonical cells, at most 65536.
		uint32_t ThicknessCells = 25;
		// Feature width as a power of two, from zero through ten.
		uint32_t FeatureShift = 5;
		// Unsigned cloud coverage code.
		uint32_t Coverage = 128;
		// Unsigned vapour mixing-ratio code.
		uint32_t Vapour = 102;
		// Linear cosmetic saturation threshold in vapour codes, with Q8 altitude/temperature slopes.
		int32_t SaturationBase = 100;
		// Signed threshold reduction per altitude cell in Q8 code units.
		int32_t SaturationLapseQ8 = 2;
		// Threshold increase per temperature anomaly code in Q8 units.
		uint32_t TemperatureSaturationQ8 = 64;
		// Maximum signed temperature anomaly magnitude in code units.
		uint32_t TemperatureVariation = 32;
		// Maximum signed pressure anomaly magnitude in code units.
		uint32_t PressureVariation = 48;
		// Bounded signed velocity disturbance magnitude in code units.
		uint32_t Turbulence = 6;
		// Signed horizontal velocity response to the layer envelope.
		int32_t Shear = 8;
		// Signed vertical velocity response to condensate.
		int32_t Lift = 4;
		// Unsigned correlation strength of moving pressure fronts.
		uint32_t FrontContrast = 77;
		// Unsigned cloud growth and decay envelope strength.
		uint32_t Growth = 64;
		// Unsigned ice fraction code.
		uint32_t Ice = 0;
		// Authored X/Y/Z velocity represented in signed profile codes.
		std::array<int32_t, 3> WindCodes{12, 0, 0};
		// Q20 cell displacement per weather tick, including the accepted time scale.
		std::array<int32_t, 3> WindQ20{};
		// Accepted phase-rate multiplier in Q16, from zero through sixteen.
		uint32_t TimeScaleQ16 = 65536;
		// Base weather clock ticks per second.
		uint32_t TickRate = 20;
		// Retains cumulative phase and displacement while accepting new clock ticks.
		bool Paused = false;
	};

	// Physical scales accompany the reference definition; they never enter integer field hashing.
	struct EncodingProfile {
		// Canonical cell edge length in metres.
		float CellSizeMetres = 64.0f;
		// Maximum condensate concentration represented by unsigned code 255.
		float CondensateKilogramsPerCubicMetre = 0.003f;
		// Maximum vapour mixing ratio represented by unsigned code 255.
		float VapourKilogramsPerKilogram = 0.03f;
		// Temperature magnitude represented by signed code 127.
		float TemperatureAnomalyCelsius = 32.0f;
		// Pressure magnitude represented by signed code 127.
		float PressureAnomalyHectopascals = 32.0f;
		// Per-axis speed magnitude represented by signed code 127.
		core::Vector3 WindScaleMetresPerSecond{64.0f, 16.0f, 64.0f};
		// Reference temperature at altitude zero.
		float BaseTemperatureCelsius = 15.0f;
		// Reference pressure at altitude zero.
		float BasePressureHectopascals = 1013.25f;
		// Temperature decrease per kilometre of altitude.
		float TemperatureLapseCelsiusPerKilometre = 6.5f;
	};

	// Both products of validated semantic compilation belong to one revision.
	struct CompiledWeather {
		// Canonical reference arithmetic inputs.
		WeatherDefinition Definition;
		// Physical meanings of the canonical byte codes.
		EncodingProfile Profile;
	};

	// Current accepted anchor is sufficient for constant-wind seeking without replay.
	struct WeatherTimelineAnchor {
		// Base weather clock tick.
		uint64_t Tick = 0;
		// Accumulated scaled phase tick, with defined uint64 wrapping.
		uint64_t PhaseTick = 0;
		// Cumulative modulo-2^32 Q8 cell displacement. This is a declared spatial drift period.
		std::array<uint32_t, 3> DisplacementQ8{};
		// Retained low twelve Q20 displacement bits, each from zero through 4095.
		std::array<uint32_t, 3> DisplacementRemainderQ12{};
		// Signed Q20 canonical-cell displacement per base clock tick.
		std::array<int32_t, 3> WindQ20{};
		// Accepted phase-rate multiplier in Q16, from zero through sixteen.
		uint32_t TimeScaleQ16 = 65536;
		// Retained fractional phase in Q16, below one whole tick.
		uint32_t PhaseFractionQ16 = 0;
		// Accepted definition/timeline revision for cache identity.
		uint32_t Revision = 1;
		// Retains cumulative phase and displacement while accepting new clock ticks.
		bool Paused = false;
	};

	// Resolved direct-seek result. No floating conversion of the absolute tick is needed.
	struct WeatherTime {
		// Base weather clock tick.
		uint64_t Tick = 0;
		// Accumulated scaled phase tick, with defined uint64 wrapping.
		uint64_t PhaseTick = 0;
		// Cumulative modular Q8 cell displacement on X/Y/Z.
		std::array<uint32_t, 3> DisplacementQ8{};
		// Retained low twelve Q20 displacement bits, each from zero through 4095.
		std::array<uint32_t, 3> DisplacementRemainderQ12{};
		// Retained fractional phase in Q16, below one whole tick.
		uint32_t PhaseFractionQ16 = 0;
		// Accepted definition/timeline revision for cache identity.
		uint32_t Revision = 1;
	};

	// Physical interpretation of a quantized cosmetic sample.
	struct PhysicalSample {
		// Maximum condensate concentration represented by unsigned code 255.
		float CondensateKilogramsPerCubicMetre = 0.0f;
		// Maximum vapour mixing ratio represented by unsigned code 255.
		float VapourKilogramsPerKilogram = 0.0f;
		// Profile-relative interpreted temperature in Celsius.
		float TemperatureCelsius = 0.0f;
		// Profile-relative interpreted pressure in hPa.
		float PressureHectopascals = 0.0f;
		// Interpreted world-space X/Y/Z velocity.
		core::Vector3 WindMetresPerSecond;
		// Interpreted frozen fraction from zero through one.
		float IceFraction = 0.0f;
	};

	// Hashes persisted layer text using uint32 FNV-1a followed by the reference avalanche hash.
	// Name intern handles and standard-library string hashing never enter this key.
	[[nodiscard]] uint32_t WeatherLayerKey(std::string_view layer);
	// Refuses nonfinite, unsupported or out-of-range authored controls; no silent saturation.
	// Default layer identity comes from LayerName text; an explicit key overrides it, including zero.
	[[nodiscard]] std::optional<CompiledWeather>
	Compile(const scene::WeatherCompute &controls, std::optional<uint32_t> layerKeyOverride = std::nullopt);
	// Refuses reserved signed codes and values outside the codec range.
	[[nodiscard]] std::optional<EncodedCell> Encode(const CellValues &values);
	// Refuses reserved signed codes instead of decoding malformed samples.
	[[nodiscard]] std::optional<CellValues> Decode(const EncodedCell &cell);
	// Fixed uint32 avalanche hash with defined modular overflow.
	[[nodiscard]] uint32_t Hash(uint32_t value);
	// Creates the accepted timeline state for an initial definition.
	[[nodiscard]] WeatherTimelineAnchor InitialAnchor(const WeatherDefinition &definition, uint64_t tick = 0);
	// Refuses seeking before the accepted anchor. Runs in constant time for every uint64 tick.
	[[nodiscard]] std::optional<WeatherTime> ResolveTime(const WeatherTimelineAnchor &anchor, uint64_t tick);
	// Replaces rate/wind only after preserving cumulative phase and displacement at the change tick.
	[[nodiscard]] std::optional<WeatherTimelineAnchor>
	Reanchor(const WeatherTimelineAnchor &anchor, const WeatherDefinition &definition, uint64_t tick);
	// Evaluates one canonical cosmetic cell using integer arithmetic exclusively.
	// Inputs must satisfy the canonical bounds established by Compile for this version.
	[[nodiscard]] EncodedCell
	Evaluate(const WeatherDefinition &definition, const WeatherTime &time, CellCoordinate coordinate);
	// All-time condensate byte-code upper bound over an inclusive canonical Y interval and every X/Z.
	// Callers include the complete sampling halo. Invalid definitions or reversed intervals are refused.
	// This certifies procedural density only; it does not certify zero vapour, wind or stateful evolution.
	[[nodiscard]] std::optional<uint8_t>
	DensityUpperBound(const WeatherDefinition &definition, int32_t minimumY, int32_t maximumY);
	// Allocation-free, order-independent batch path; mismatched spans are refused before any write.
	[[nodiscard]] bool EvaluateBatch(
		const WeatherDefinition &definition,
		const WeatherTime &time,
		std::span<const CellCoordinate> coordinates,
		std::span<EncodedCell> output
	);
	// Converts byte codes to declared physical units at the queried altitude.
	[[nodiscard]] std::optional<PhysicalSample>
	ToPhysical(const EncodedCell &cell, const EncodingProfile &profile, float altitudeMetres);
	// Named optics conversion: droplet-specific mass extinction times condensate concentration.
	[[nodiscard]] float ExtinctionPerMetre(
		float condensateKilogramsPerCubicMetre, float massExtinctionSquareMetresPerKilogram = 150.0f
	);
	// Water budget in kilograms under an explicit constant dry-air density approximation.
	[[nodiscard]] float WaterKilograms(
		const PhysicalSample &sample, float volumeCubicMetres, float dryAirKilogramsPerCubicMetre = 1.225f
	);
}
