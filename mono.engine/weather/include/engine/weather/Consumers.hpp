#pragma once

// Shared headless weather queries. These use the reference field, never GPU readback.
// Precipitation rates are water-equivalent mm/hour; fog is a separate near-ground medium.
// @tier L8 · shared

#include <engine/weather/Weather.hpp>

#include <optional>

namespace engine::scene {
	struct Atmosphere;
	struct GpuParticleField;
}

namespace engine::weather {
	// Copied emission inputs shared by gameplay and cosmetic particle adapters.
	struct PrecipitationSample {
		// Water-equivalent rates; liquid plus snow plus hail equals the authored column rate.
		//@{
		float LiquidMillimetresPerHour = 0.0f;
		float SnowMillimetresPerHour = 0.0f;
		float HailMillimetresPerHour = 0.0f;
		//@}
		// Fraction of source ice melted while falling through the warm column.
		float MeltingFraction = 0.0f;
		// Canonical seed excludes cameras, GPU slots and visual particle counts.
		uint32_t SpawnSeed = 0;
	};

	// One physical sample and its explicitly simplified consumer responses.
	struct ConsumerSample {
		// Thermodynamic sample at the query's world altitude.
		PhysicalSample Air;
		// Integrated column precipitation, independent of visual condensate density.
		PrecipitationSample Precipitation;
		// Terrain-relative fog extinction in inverse metres; molecular scattering is separate.
		float FogExtinctionPerMetre = 0.0f;
		// Bounded storm/ice/updraft proxy, not an electrical circuit simulation.
		float ChargeSeparation = 0.0f;
		// Bounded cold updraft retention proxy used for hail growth.
		float HailGrowth = 0.0f;
	};

	// Explicit precedence per atmosphere property. Other authored appearance is preserved.
	enum class ResponsePrecedence : uint8_t {
		// Preserve the inherited authored control exactly.
		Authored,
		// Apply the explicit weather response to the authored control.
		Weather
	};

	// Pure response curves. Zero strengths preserve every inherited authored control.
	struct AtmosphereResponse {
		// Density, haze and glare have independent precedence.
		//@{
		ResponsePrecedence DensityPrecedence = ResponsePrecedence::Authored;
		ResponsePrecedence HazePrecedence = ResponsePrecedence::Authored;
		ResponsePrecedence GlarePrecedence = ResponsePrecedence::Authored;
		//@}
		// Bounded linear response coefficients for moisture, storm and glare attenuation.
		//@{
		float DensityStrength = 0.0f;
		float HazeStrength = 0.0f;
		float GlareSuppression = 0.0f;
		//@}
	};

	// Tetens water saturation, epsilon*e/(p-e), kg vapour/kg dry air. Refuses invalid pressure.
	[[nodiscard]] std::optional<float>
	SaturationMixingRatio(float temperatureCelsius, float pressureHectopascals);
	// Direct-seek query with 16 fixed column samples and caller-supplied terrain surface altitude.
	[[nodiscard]] std::optional<ConsumerSample> SampleConsumers(
		const CompiledWeather &compiled,
		const WeatherTime &time,
		const scene::WeatherCompute &controls,
		core::Vector3 positionMetres,
		float groundHeightMetres = 0.0f
	);
	// Applies only weather-selected response properties to a copied authored atmosphere.
	[[nodiscard]] scene::Atmosphere ApplyAtmosphereResponse(
		const scene::Atmosphere &authored, const ConsumerSample &sample, const AtmosphereResponse &response
	);
	// Cosmetic visibility multiplier. Occlusion never alters the authoritative sample rate.
	[[nodiscard]] PrecipitationSample VisiblePrecipitation(
		const PrecipitationSample &sample, bool occluded, float distanceMetres, float maximumDistanceMetres
	);
	// Pure adapter to the existing generic GPU field. Groups are liquid, snow and hail.
	// Ninety-six weighted spawn samples preserve rate ratios; occlusion is already applied by caller.
	[[nodiscard]] std::optional<scene::GpuParticleField> BuildPrecipitationParticles(
		const ConsumerSample &sample, core::Vector3 halfExtentMetres, uint32_t maximumParticles = 1048576
	);

}
