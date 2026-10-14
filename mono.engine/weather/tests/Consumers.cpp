#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Consumers.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.weather.consumers")

using namespace engine;
using Catch::Approx;

namespace {
	weather::ConsumerSample
	Query(scene::WeatherCompute controls, core::Vector3 position = {}, float ground = 0) {
		const auto compiled = weather::Compile(controls, 18);
		REQUIRE(compiled);
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
		REQUIRE(time);
		const auto sample = weather::SampleConsumers(*compiled, *time, controls, position, ground);
		REQUIRE(sample);
		return *sample;
	}
}

TEST_CASE("precipitation preserves authored water-equivalent rate through a melting column", "[weather]") {
	scene::WeatherCompute controls;
	controls.PrecipitationRate = 12;
	controls.Coverage = 0;
	controls.IceFraction = 1;
	controls.TemperatureVariationCelsius = 0;
	controls.TemperatureLapseCelsiusPerKilometre = 0;
	controls.BaseTemperatureCelsius = -10;
	const auto cold = Query(controls);
	CHECK(cold.Air.CondensateKilogramsPerCubicMetre == 0);
	CHECK(cold.Precipitation.SnowMillimetresPerHour == Approx(12));
	CHECK(cold.Precipitation.LiquidMillimetresPerHour == 0);
	controls.BaseTemperatureCelsius = 20;
	const auto warm = Query(controls);
	CHECK(warm.Precipitation.MeltingFraction > 0.99f);
	CHECK(warm.Precipitation.LiquidMillimetresPerHour > 11.9f);
	CHECK(
		warm.Precipitation.LiquidMillimetresPerHour + warm.Precipitation.SnowMillimetresPerHour +
			warm.Precipitation.HailMillimetresPerHour ==
		Approx(12)
	);
	CHECK(Query(controls, {0, 3000, 0}).Precipitation.LiquidMillimetresPerHour == 0);
	CHECK(Query(controls, {0, -1, 0}).Precipitation.LiquidMillimetresPerHour == 0);
}

TEST_CASE("terrain-relative fog follows cooled saturation and clear layers stay dry", "[weather]") {
	scene::WeatherCompute controls;
	controls.FogStrength = 1;
	controls.VapourMixingRatio = 0.03f;
	controls.TemperatureVariationCelsius = 0;
	controls.BaseTemperatureCelsius = 4;
	const auto valley = Query(controls, {10, 100, 20}, 100);
	CHECK(valley.FogExtinctionPerMetre > 0);
	CHECK(Query(controls, {10, 164, 20}, 100).FogExtinctionPerMetre == 0);
	CHECK(Query(controls, {10, 100, 20}, 0).FogExtinctionPerMetre == 0);
	controls.BaseTemperatureCelsius = 60;
	CHECK(Query(controls).FogExtinctionPerMetre == 0);
	controls.Enabled = false;
	controls.PrecipitationRate = 20;
	const auto disabled = Query(controls);
	CHECK(disabled.FogExtinctionPerMetre == 0);
	CHECK(disabled.ChargeSeparation == 0);
	CHECK(disabled.Precipitation.LiquidMillimetresPerHour == 0);
}

TEST_CASE("hail requires cold ice and bounded updrafts while lightning retains storm charge", "[weather]") {
	scene::WeatherCompute controls;
	controls.BaseTemperatureCelsius = -20;
	controls.TemperatureVariationCelsius = 0;
	controls.PrecipitationRate = 20;
	controls.IceFraction = 1;
	controls.HailIntensity = 1;
	controls.StormIntensity = 1;
	controls.WindMetresPerSecond.Y = 8;
	controls.LiftMetresPerSecond = 0;
	controls.TurbulenceMetresPerSecond = 0;
	const auto strong = Query(controls);
	CHECK(strong.HailGrowth == Approx(1));
	CHECK(strong.ChargeSeparation == Approx(1));
	CHECK(strong.Precipitation.HailMillimetresPerHour == Approx(20));
	controls.WindMetresPerSecond.Y = 0;
	CHECK(Query(controls).HailGrowth == 0);
	CHECK(Query(controls).ChargeSeparation == 0);
}

TEST_CASE(
	"atmosphere precedence and cosmetic occlusion preserve authored and authoritative inputs", "[weather]"
) {
	scene::Atmosphere authored;
	authored.Density = 0.2f;
	authored.Haze = 1;
	authored.Glare = 4;
	authored.Offset = 0.8f;
	weather::ConsumerSample sample;
	sample.Air.VapourKilogramsPerKilogram = 0.03f;
	sample.ChargeSeparation = 0.5f;
	weather::AtmosphereResponse response;
	response.DensityStrength = 1;
	CHECK(weather::ApplyAtmosphereResponse(authored, sample, response).Density == authored.Density);
	response.DensityPrecedence = weather::ResponsePrecedence::Weather;
	response.HazePrecedence = weather::ResponsePrecedence::Weather;
	response.GlarePrecedence = weather::ResponsePrecedence::Weather;
	response.HazeStrength = 0.5f;
	response.GlareSuppression = 1;
	const auto changed = weather::ApplyAtmosphereResponse(authored, sample, response);
	CHECK(changed.Density == 1);
	CHECK(changed.Haze == Approx(3.5f));
	CHECK(changed.Glare == Approx(2));
	CHECK(changed.Colour == authored.Colour);
	CHECK(changed.Decay == authored.Decay);
	CHECK(changed.Offset == authored.Offset);
	weather::PrecipitationSample rain;
	rain.LiquidMillimetresPerHour = 8;
	rain.SpawnSeed = 71;
	CHECK(weather::VisiblePrecipitation(rain, true, 0, 100).LiquidMillimetresPerHour == 0);
	CHECK(weather::VisiblePrecipitation(rain, false, 50, 100).LiquidMillimetresPerHour == 4);
	CHECK(weather::VisiblePrecipitation(rain, false, 100, 100).LiquidMillimetresPerHour == 0);
	CHECK(rain.LiquidMillimetresPerHour == 8);
}

TEST_CASE("consumer boundary refuses malformed coordinates and incompatible revisions", "[weather]") {
	const auto saturation = weather::SaturationMixingRatio(20, 1013.25f);
	REQUIRE(saturation);
	CHECK(*saturation == Approx(0.0147f).margin(0.0002f));
	CHECK_FALSE(weather::SaturationMixingRatio(20, 1));
	CHECK_FALSE(weather::SaturationMixingRatio(100, 1013));
	scene::WeatherCompute controls;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), UINT64_MAX);
	REQUIRE(time);
	CHECK_FALSE(
		weather::SampleConsumers(*compiled, *time, controls, {std::numeric_limits<float>::infinity(), 0, 0})
	);
	CHECK_FALSE(
		weather::SampleConsumers(*compiled, *time, controls, {std::numeric_limits<float>::max(), 0, 0})
	);
	++time->Revision;
	CHECK_FALSE(weather::SampleConsumers(*compiled, *time, controls, {}));
}

TEST_CASE("batched consumer columns preserve scalar physical responses over sloped terrain", "[weather]") {
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 32;
	controls.BaseHeightMetres = 700;
	controls.ThicknessMetres = 1600;
	controls.BaseTemperatureCelsius = 7;
	controls.TemperatureVariationCelsius = 8;
	controls.TemperatureLapseCelsiusPerKilometre = 9;
	controls.WindMetresPerSecond = {3, 12, -4};
	controls.TurbulenceMetresPerSecond = 10;
	controls.LiftMetresPerSecond = 4;
	controls.IceFraction = 0.8f;
	controls.StormIntensity = 0.6f;
	controls.HailIntensity = 0.7f;
	controls.PrecipitationRate = 13;
	controls.FogStrength = 1;
	const auto compiled = weather::Compile(controls, 99);
	REQUIRE(compiled);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{71}, UINT64_MAX - 5}) {
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		for (const core::Vector3 horizontal :
			 {core::Vector3{-1300.5f, 0, 641.75f}, core::Vector3{31.99f, 0, -32.01f}}) {
			const float ground = horizontal.X * -0.01f + horizontal.Z * 0.02f;
			const core::Vector3 position{horizontal.X, ground + 8, horizontal.Z};
			const auto scalar = [&](float altitude) {
				const weather::CellCoordinate coordinate{
					static_cast<int32_t>(
						std::floor(static_cast<double>(position.X) / controls.CellSizeMetres)
					),
					static_cast<int32_t>(std::floor(static_cast<double>(altitude) / controls.CellSizeMetres)),
					static_cast<int32_t>(
						std::floor(static_cast<double>(position.Z) / controls.CellSizeMetres)
					)
				};
				const auto physical = weather::ToPhysical(
					weather::Evaluate(compiled->Definition, *time, coordinate), compiled->Profile, altitude
				);
				REQUIRE(physical);
				return *physical;
			};
			const float sourceHeight = controls.BaseHeightMetres + controls.ThicknessMetres * 0.75f;
			const auto source = scalar(sourceHeight);
			const float length = sourceHeight - position.Y;
			float warmDegreeMetres = 0;
			float maximumIce = source.IceFraction;
			float maximumUpdraft = std::max(0.0f, source.WindMetresPerSecond.Y);
			for (uint32_t index = 0; index < 16; ++index) {
				const float altitude = position.Y + length * ((static_cast<float>(index) + 0.5f) / 16);
				const auto cell = scalar(altitude);
				warmDegreeMetres += std::max(0.0f, cell.TemperatureCelsius) * length / 16;
				maximumIce = std::max(maximumIce, cell.IceFraction);
				maximumUpdraft = std::max(maximumUpdraft, cell.WindMetresPerSecond.Y);
			}
			const float melting = 1 - std::exp(-warmDegreeMetres / 1000);
			const float updraft = std::clamp(maximumUpdraft / 8, 0.0f, 1.0f);
			const auto sample = weather::SampleConsumers(*compiled, *time, controls, position, ground);
			REQUIRE(sample);
			CHECK(
				std::bit_cast<uint32_t>(sample->Precipitation.MeltingFraction) ==
				std::bit_cast<uint32_t>(melting)
			);
			CHECK(
				std::bit_cast<uint32_t>(sample->ChargeSeparation) ==
				std::bit_cast<uint32_t>(controls.StormIntensity * maximumIce * updraft)
			);
			CHECK(
				std::bit_cast<uint32_t>(sample->HailGrowth) ==
				std::bit_cast<uint32_t>(controls.HailIntensity * maximumIce * updraft * (1 - melting))
			);
			const auto air = scalar(position.Y);
			const float suspension = 1 - (position.Y - ground) / 64;
			const auto saturation = weather::SaturationMixingRatio(
				air.TemperatureCelsius - 3 * suspension, air.PressureHectopascals
			);
			REQUIRE(saturation);
			const float fog = weather::ExtinctionPerMetre(
								  std::max(0.0f, air.VapourKilogramsPerKilogram - *saturation) * 1.225f
							  ) *
							  controls.FogStrength * suspension;
			CHECK(std::bit_cast<uint32_t>(sample->FogExtinctionPerMetre) == std::bit_cast<uint32_t>(fog));
		}
	}
}

TEST_CASE("cosmetic precipitation packets preserve rate ratios and stable spawn identity", "[weather]") {
	weather::ConsumerSample sample;
	sample.Precipitation.LiquidMillimetresPerHour = 6;
	sample.Precipitation.SnowMillimetresPerHour = 3;
	sample.Precipitation.HailMillimetresPerHour = 3;
	sample.Precipitation.SpawnSeed = 912;
	sample.Air.WindMetresPerSecond = {4, 0, -2};
	const auto field = weather::BuildPrecipitationParticles(sample, {32, 64, 32});
	REQUIRE(field);
	CHECK(scene::ValidGpuParticleField(*field));
	CHECK(field->Enabled);
	CHECK(field->Layers == 7);
	CHECK(field->SpawnSamples.size() == 96);
	std::array<uint32_t, 3> counts{};
	for (const auto &spawn : field->SpawnSamples) {
		++counts[spawn.Layer];
		CHECK(spawn.Velocity.X == 4);
		CHECK(spawn.Velocity.Z == -2);
		CHECK(spawn.Velocity.Y < 0);
	}
	CHECK(counts == std::array<uint32_t, 3>{48, 24, 24});
	const auto repeated = weather::BuildPrecipitationParticles(sample, {32, 64, 32});
	REQUIRE(repeated);
	CHECK(field->SpawnSamples == repeated->SpawnSamples);
	sample.Precipitation = weather::VisiblePrecipitation(sample.Precipitation, true, 0, 100);
	const auto indoors = weather::BuildPrecipitationParticles(sample, {32, 64, 32});
	REQUIRE(indoors);
	CHECK_FALSE(indoors->Enabled);
	CHECK(indoors->SpawnSamples.empty());
	CHECK_FALSE(weather::BuildPrecipitationParticles(sample, {-1, 64, 32}));
	CHECK_FALSE(weather::BuildPrecipitationParticles(sample, {32, 64, 32}, 100));
}
