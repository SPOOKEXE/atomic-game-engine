#include <engine/core/Profiling.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/weather/Consumers.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::weather {
	namespace {
		constexpr uint32_t COLUMN_SAMPLES = 16;
		constexpr float FOG_HEIGHT_METRES = 64.0f;
		constexpr float SURFACE_COOLING_CELSIUS = 3.0f;
		constexpr float MELTING_DEGREE_METRES = 1000.0f;
		constexpr float HAIL_RETENTION_UPDRAFT_METRES_PER_SECOND = 8.0f;

		std::optional<CellCoordinate> Coordinate(core::Vector3 position, float cellSize) {
			if (!std::isfinite(cellSize) || cellSize <= 0) return std::nullopt;
			CellCoordinate coordinate;
			const float axes[] = {position.X, position.Y, position.Z};
			int32_t *destinations[] = {&coordinate.X, &coordinate.Y, &coordinate.Z};
			for (uint32_t axis = 0; axis < 3; ++axis) {
				const double cell = std::floor(static_cast<double>(axes[axis]) / cellSize);
				if (!std::isfinite(cell) || cell < INT32_MIN || cell > INT32_MAX) return std::nullopt;
				*destinations[axis] = static_cast<int32_t>(cell);
			}
			return coordinate;
		}

		std::optional<PhysicalSample>
		Sample(const CompiledWeather &compiled, const WeatherTime &time, core::Vector3 position) {
			const auto coordinate = Coordinate(position, compiled.Profile.CellSizeMetres);
			if (!coordinate) return std::nullopt;
			return ToPhysical(Evaluate(compiled.Definition, time, *coordinate), compiled.Profile, position.Y);
		}

		float Strength(float strength) {
			return std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
		}
	}

	std::optional<float> SaturationMixingRatio(float temperatureCelsius, float pressureHectopascals) {
		if (!std::isfinite(temperatureCelsius) || temperatureCelsius < -100.0f ||
			temperatureCelsius > 60.0f || !std::isfinite(pressureHectopascals) || pressureHectopascals <= 0)
			return std::nullopt;
		// ECMWF Tetens water coefficients: T in K, e in Pa. This bounded curve is not a solver.
		const float temperatureKelvin = temperatureCelsius + 273.15f;
		const float vapourPressurePa =
			611.21f * std::exp(17.502f * (temperatureKelvin - 273.16f) / (temperatureKelvin - 32.19f));
		const float pressurePa = pressureHectopascals * 100.0f;
		if (vapourPressurePa >= pressurePa) return std::nullopt;
		return 0.622f * vapourPressurePa / (pressurePa - vapourPressurePa);
	}

	std::optional<ConsumerSample> SampleConsumers(
		const CompiledWeather &compiled,
		const WeatherTime &time,
		const scene::WeatherCompute &controls,
		core::Vector3 positionMetres,
		float groundHeightMetres
	) {
		ENGINE_PROFILE("weather consumers");
		if (!scene::ValidWeatherCompute(controls) || !std::isfinite(groundHeightMetres) ||
			time.Revision != compiled.Definition.Revision)
			return std::nullopt;
		const auto air = Sample(compiled, time, positionMetres);
		if (!air) return std::nullopt;
		ConsumerSample result;
		result.Air = *air;
		if (!controls.Enabled) return result;
		const auto coordinate = Coordinate(positionMetres, compiled.Profile.CellSizeMetres);
		if (!coordinate) return std::nullopt;
		result.Precipitation.SpawnSeed = Hash(
			compiled.Definition.Seed ^ Hash(compiled.Definition.LayerKey) ^
			Hash(static_cast<uint32_t>(coordinate->X)) ^ Hash(static_cast<uint32_t>(coordinate->Z)) ^
			Hash(static_cast<uint32_t>(time.Tick)) ^ Hash(static_cast<uint32_t>(time.Tick >> 32u))
		);
		const float top = controls.BaseHeightMetres + controls.ThicknessMetres;
		const float sourceHeight = controls.BaseHeightMetres + controls.ThicknessMetres * 0.75f;
		const auto source = Sample(compiled, time, {positionMetres.X, sourceHeight, positionMetres.Z});
		if (!source) return std::nullopt;
		const float columnLength = std::max(0.0f, sourceHeight - positionMetres.Y);
		float warmDegreeMetres = 0.0f;
		float maximumIce = source->IceFraction;
		float maximumUpdraft = std::max(0.0f, source->WindMetresPerSecond.Y);
		std::array<float, COLUMN_SAMPLES> altitudes;
		std::array<CellCoordinate, COLUMN_SAMPLES> columnCoordinates;
		std::array<EncodedCell, COLUMN_SAMPLES> columnCells;
		for (uint32_t index = 0; index < COLUMN_SAMPLES; ++index) {
			const float fraction = (static_cast<float>(index) + 0.5f) / COLUMN_SAMPLES;
			const float altitude = positionMetres.Y + columnLength * fraction;
			const double cellY = std::floor(static_cast<double>(altitude) / compiled.Profile.CellSizeMetres);
			if (!std::isfinite(cellY) || cellY < INT32_MIN || cellY > INT32_MAX) return std::nullopt;
			altitudes[index] = altitude;
			columnCoordinates[index] = {coordinate->X, static_cast<int32_t>(cellY), coordinate->Z};
		}
		if (!EvaluateBatch(compiled.Definition, time, columnCoordinates, columnCells)) return std::nullopt;
		for (uint32_t index = 0; index < COLUMN_SAMPLES; ++index) {
			const auto column = ToPhysical(columnCells[index], compiled.Profile, altitudes[index]);
			if (!column) return std::nullopt;
			warmDegreeMetres += std::max(0.0f, column->TemperatureCelsius) * columnLength / COLUMN_SAMPLES;
			maximumIce = std::max(maximumIce, column->IceFraction);
			maximumUpdraft = std::max(maximumUpdraft, column->WindMetresPerSecond.Y);
		}
		const float frozenSource =
			std::max(source->IceFraction, std::clamp(-source->TemperatureCelsius / 5.0f, 0.0f, 1.0f));
		const float melting = 1.0f - std::exp(-warmDegreeMetres / MELTING_DEGREE_METRES);
		result.Precipitation.MeltingFraction = melting;
		const float updraft =
			std::clamp(maximumUpdraft / HAIL_RETENTION_UPDRAFT_METRES_PER_SECOND, 0.0f, 1.0f);
		result.ChargeSeparation = controls.StormIntensity * maximumIce * updraft;
		result.HailGrowth = controls.HailIntensity * maximumIce * updraft * (1.0f - melting);
		const float rate = positionMetres.Y >= top || positionMetres.Y < groundHeightMetres
							   ? 0.0f
							   : controls.PrecipitationRate;
		const float hailFraction = std::min(result.HailGrowth, frozenSource * (1.0f - melting));
		result.Precipitation.HailMillimetresPerHour = rate * hailFraction;
		result.Precipitation.SnowMillimetresPerHour =
			rate * std::max(0.0f, frozenSource * (1.0f - melting) - hailFraction);
		result.Precipitation.LiquidMillimetresPerHour = std::max(
			0.0f,
			rate - result.Precipitation.HailMillimetresPerHour - result.Precipitation.SnowMillimetresPerHour
		);
		const float heightAboveGround = positionMetres.Y - groundHeightMetres;
		if (heightAboveGround < 0 || heightAboveGround > FOG_HEIGHT_METRES) return result;
		const float suspension = 1.0f - heightAboveGround / FOG_HEIGHT_METRES;
		const float surfaceTemperature = air->TemperatureCelsius - SURFACE_COOLING_CELSIUS * suspension;
		const auto saturation = SaturationMixingRatio(surfaceTemperature, air->PressureHectopascals);
		if (!saturation) return result;
		// Constant dry-air density, kg/m^3: excess mixing ratio becomes suspended condensate.
		const float excess = std::max(0.0f, air->VapourKilogramsPerKilogram - *saturation);
		result.FogExtinctionPerMetre =
			ExtinctionPerMetre(excess * 1.225f) * controls.FogStrength * suspension;
		return result;
	}

	scene::Atmosphere ApplyAtmosphereResponse(
		const scene::Atmosphere &authored, const ConsumerSample &sample, const AtmosphereResponse &response
	) {
		scene::Atmosphere result = authored;
		const float moisture = std::clamp(sample.Air.VapourKilogramsPerKilogram / 0.03f, 0.0f, 1.0f);
		if (response.DensityPrecedence == ResponsePrecedence::Weather)
			result.Density =
				std::clamp(authored.Density + moisture * Strength(response.DensityStrength), 0.0f, 1.0f);
		if (response.HazePrecedence == ResponsePrecedence::Weather)
			result.Haze = std::clamp(
				authored.Haze + sample.ChargeSeparation * Strength(response.HazeStrength) * 10.0f, 0.0f, 10.0f
			);
		if (response.GlarePrecedence == ResponsePrecedence::Weather)
			result.Glare =
				authored.Glare * (1.0f - sample.ChargeSeparation * Strength(response.GlareSuppression));
		return result;
	}

	PrecipitationSample VisiblePrecipitation(
		const PrecipitationSample &sample, bool occluded, float distanceMetres, float maximumDistanceMetres
	) {
		PrecipitationSample result = sample;
		const float visibility = occluded || !std::isfinite(distanceMetres) || distanceMetres < 0 ||
										 !std::isfinite(maximumDistanceMetres) || maximumDistanceMetres <= 0
									 ? 0.0f
									 : std::clamp(1.0f - distanceMetres / maximumDistanceMetres, 0.0f, 1.0f);
		result.LiquidMillimetresPerHour *= visibility;
		result.SnowMillimetresPerHour *= visibility;
		result.HailMillimetresPerHour *= visibility;
		return result;
	}
	std::optional<scene::GpuParticleField> BuildPrecipitationParticles(
		const ConsumerSample &sample, core::Vector3 halfExtentMetres, uint32_t maximumParticles
	) {
		if (maximumParticles > 0 && maximumParticles < 1048576) return std::nullopt;
		const auto &rain = sample.Precipitation;
		const std::array<float, 3> rates{
			rain.LiquidMillimetresPerHour, rain.SnowMillimetresPerHour, rain.HailMillimetresPerHour
		};
		for (const float rate : rates)
			if (!std::isfinite(rate) || rate < 0 || rate > 1000) return std::nullopt;
		scene::GpuParticleField field;
		field.HalfExtent = halfExtentMetres;
		field.Seed = rain.SpawnSeed;
		field.VelocityResponse = 0;
		field.Layers = 0;
		const float total = rates[0] + rates[1] + rates[2];
		field.Enabled = total > 0 && maximumParticles > 0;
		// The field uses its existing population presets; opacity expresses sub-preset precipitation.
		const uint32_t requested = static_cast<uint32_t>(
			std::ceil(std::min(total * 4096.0f, static_cast<float>(std::min(maximumParticles, 50000000u))))
		);
		uint32_t presetRequest = std::max(1u, requested);
		field.RequestedCount = scene::NormalizeGpuParticleCount(presetRequest);
		while (maximumParticles > 0 && field.RequestedCount > maximumParticles) {
			presetRequest /= 2;
			field.RequestedCount = scene::NormalizeGpuParticleCount(presetRequest);
		}
		const float populationScale =
			field.RequestedCount == 0
				? 0.0f
				: std::min(1.0f, static_cast<float>(requested) / static_cast<float>(field.RequestedCount));
		field.Styles[0] = {{0.6f, 0.75f, 1.0f}, 0.35f * populationScale, 0.025f, {}};
		field.Styles[1] = {{1, 1, 1}, 0.65f * populationScale, 0.09f, {}};
		field.Styles[2] = {{0.9f, 0.95f, 1}, 0.8f * populationScale, 0.045f, {}};
		if (!scene::ValidGpuParticleField(field)) return std::nullopt;
		if (!field.Enabled) return field;
		constexpr uint32_t SPAWN_SAMPLES = 96;
		const std::array<float, 3> terminalSpeeds{9.0f, 1.5f, 18.0f};
		field.SpawnSamples.reserve(SPAWN_SAMPLES);
		for (uint32_t index = 0; index < SPAWN_SAMPLES; ++index) {
			const float quantile = (static_cast<float>(index) + 0.5f) / SPAWN_SAMPLES * total;
			const uint32_t group = quantile < rates[0] ? 0 : quantile < rates[0] + rates[1] ? 1 : 2;
			field.Layers |= static_cast<uint8_t>(1u << group);
			const uint32_t seed = Hash(rain.SpawnSeed ^ Hash(index));
			const auto unit = [](uint32_t random) { return static_cast<float>(random & 65535u) / 65535.0f; };
			const core::Vector3 position{
				(unit(Hash(seed ^ 0x58524149u)) * 2 - 1) * halfExtentMetres.X,
				(unit(Hash(seed ^ 0x59524149u)) * 2 - 1) * halfExtentMetres.Y,
				(unit(Hash(seed ^ 0x5a524149u)) * 2 - 1) * halfExtentMetres.Z
			};
			const core::Vector3 velocity{
				sample.Air.WindMetresPerSecond.X, -terminalSpeeds[group], sample.Air.WindMetresPerSecond.Z
			};
			const float lifetime = std::clamp(2 * halfExtentMetres.Y / terminalSpeeds[group], 0.01f, 3600.0f);
			field.SpawnSamples.push_back({position, lifetime, velocity, group});
		}
		if (!scene::ValidGpuParticleField(field)) return std::nullopt;
		return field;
	}

}
