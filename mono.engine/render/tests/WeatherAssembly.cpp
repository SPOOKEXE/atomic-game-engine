#include "WeatherAssembly.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.weatherassembly")
TEST_DEPENDS("engine.weather.assemblies")

TEST_CASE(
	"full-page classification keeps dense fronts and compresses uniform coarse samples", "[render][weather]"
) {
	using namespace engine;
	scene::WeatherCompute controls;
	controls.FeatureCells = 1024;
	controls.Coverage = 0.02f;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.TemperatureVariationCelsius = 0;
	controls.PressureVariationHectopascals = 0;
	controls.TurbulenceMetresPerSecond = 0;
	controls.ShearMetresPerSecond = 0;
	controls.LiftMetresPerSecond = 0;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 0);
	REQUIRE(time);
	CHECK(render::WeatherPageCanCompress(*compiled, *time, {{0, 0, 0}, 1}) == true);
	CHECK(render::WeatherPageCanCompress(*compiled, *time, {{0, 0, 0}, 0}) == false);
	CHECK_FALSE(render::WeatherPageCanCompress(*compiled, *time, {{1, 0, 0}, 1}));
	CHECK(render::WeatherPageIsEmpty(*compiled, {{0, 0, 0}, 1}) == false);
	weather::AssemblyThresholds strict;
	strict.MaximumVariance.fill(0);
	strict.MaximumContrast.fill(0);
	auto front = *compiled;
	front.Definition.FeatureShift = 2;
	front.Definition.PressureVariation = 127;
	CHECK(render::WeatherPageCanCompress(front, *time, {{0, 0, 0}, 1}, strict) == false);
}

TEST_CASE("empty page proofs include all reconstruction neighbours across ticks", "[render][weather]") {
	using namespace engine;
	weather::CompiledWeather compiled;
	compiled.Definition.BaseCell = 16;
	compiled.Definition.ThicknessCells = 16;
	compiled.Definition.Coverage = 255;
	compiled.Definition.Vapour = 255;
	compiled.Definition.TemperatureVariation = 0;
	compiled.Definition.SaturationBase = 0;
	compiled.Definition.SaturationLapseQ8 = 0;
	const std::array<weather::BrickKey, 4> pages{
		{{{0, -16, 0}, 1}, {{0, 32, 0}, 1}, {{0, 0, 0}, 1}, {{0, 0, 0}, 4}}
	};
	CHECK(render::WeatherPageIsEmpty(compiled, pages[0]) == true);
	// Both pages touch the finite layer through their interpolation halo.
	CHECK(render::WeatherPageIsEmpty(compiled, pages[1]) == false);
	CHECK(render::WeatherPageIsEmpty(compiled, pages[2]) == false);
	CHECK(render::WeatherPageIsEmpty(compiled, pages[3]) == false);
	CHECK_FALSE(render::WeatherPageIsEmpty(compiled, {{1, 0, 0}, 1}));
	for (const uint32_t mode : {0u, 1u, 2u}) {
		auto empty = compiled;
		if (mode == 0) empty.Definition.Coverage = 0;
		if (mode == 1) {
			empty.Definition.Vapour = 0;
			empty.Definition.SaturationBase = 255;
		}
		const auto page = mode == 2 ? pages[0] : pages[3];
		REQUIRE(render::WeatherPageIsEmpty(empty, page) == true);
		const int32_t side = int32_t{8} << page.Lod;
		for (const uint64_t tick : {0ull, 17ull, 1000ull, 1000000ull}) {
			const auto time = weather::ResolveTime(weather::InitialAnchor(empty.Definition), tick);
			REQUIRE(time);
			for (int32_t y = page.Origin.Y - 1; y <= page.Origin.Y + side + 1; ++y)
				for (int32_t z = -1; z <= 8; ++z)
					for (int32_t x = -1; x <= 8; ++x)
						CHECK(weather::Evaluate(empty.Definition, *time, {x * 13, y, z * 19}).Codes[0] == 0);
		}
	}
}

TEST_CASE("packed GPU assembly preserves five byte-code totals and every child wind", "[render][weather]") {
	using namespace engine;
	for (uint32_t seed = 0; seed < 256; ++seed) {
		std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN> cells{};
		for (size_t child = 0; child < cells.size(); ++child) {
			weather::CellValues sample;
			sample.Condensate = static_cast<uint8_t>(weather::Hash(seed + static_cast<uint32_t>(child)));
			sample.Vapour = static_cast<uint8_t>(weather::Hash(seed ^ static_cast<uint32_t>(child)));
			sample.TemperatureAnomaly = static_cast<int16_t>(static_cast<int32_t>(seed % 255) - 127);
			sample.PressureAnomaly = static_cast<int16_t>(127 - static_cast<int32_t>(child * 31));
			sample.Ice = static_cast<uint8_t>(seed + static_cast<uint32_t>(child));
			sample.Wind = {
				static_cast<int16_t>(child * 16), static_cast<int16_t>(-static_cast<int32_t>(child) * 16), 127
			};
			cells[child] = *weather::Encode(sample);
		}
		const auto packed = render::PackWeatherAssembly(cells);
		REQUIRE(packed);
		const auto restored = render::UnpackWeatherAssembly(*packed);
		REQUIRE(restored);
		const auto original = weather::Summarize({}, {}, cells);
		const auto reconstructed = weather::Summarize({}, {}, *restored);
		REQUIRE(original);
		REQUIRE(reconstructed);
		for (size_t scalar = 0; scalar < weather::ASSEMBLY_SCALARS; ++scalar)
			CHECK(reconstructed->Scalars[scalar].Sum == original->Scalars[scalar].Sum);
		CHECK(reconstructed->ChildWind == original->ChildWind);
		CHECK(render::PackWeatherAssembly(*restored) == packed);
	}
	CHECK(render::WEATHER_COMPRESSED_PAGE_BYTES == 2048);
	CHECK(weather::BRICK_PAYLOAD_BYTES == 4096);
}

TEST_CASE(
	"spatial assembly indexing maps all 512 cells to 64 aligned eight-child groups", "[render][weather]"
) {
	std::array<uint8_t, 512> visits{};
	for (uint32_t z = 0; z < 8; ++z)
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x) {
				const auto address = engine::render::WeatherAssemblyIndex(x, y, z);
				REQUIRE(address[0] < 64);
				REQUIRE(address[1] < 8);
				++visits[address[0] * 8 + address[1]];
			}
	for (const auto visitsAtAddress : visits)
		CHECK(visitsAtAddress == 1);
}

TEST_CASE(
	"packed assembly rejects reserved codes unused bits and impossible exact totals", "[render][weather]"
) {
	using namespace engine;
	std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN> cells{};
	weather::CellValues sample;
	sample.Condensate = 255;
	sample.TemperatureAnomaly = 127;
	sample.PressureAnomaly = -127;
	cells.fill(*weather::Encode(sample));
	const auto packed = render::PackWeatherAssembly(cells);
	REQUIRE(packed);
	CHECK(render::UnpackWeatherAssembly(*packed) == cells);
	auto invalid = *packed;
	invalid.Words[1] |= 0x80000000u;
	CHECK_FALSE(render::UnpackWeatherAssembly(invalid));
	invalid = *packed;
	invalid.Words[0] |= 2047;
	CHECK_FALSE(render::UnpackWeatherAssembly(invalid));
	invalid = *packed;
	invalid.Words[2] |= 255;
	CHECK_FALSE(render::UnpackWeatherAssembly(invalid));
	cells[0].Codes[1] = 255;
	CHECK_FALSE(render::PackWeatherAssembly(cells));
}
