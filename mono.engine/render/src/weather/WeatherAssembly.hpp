#pragma once

// Private CPU reference for the GPU's 32-byte eight-cell assembly codec.
// Five exact scalar sums share two words; every original child wind byte survives.

#include <engine/weather/Assemblies.hpp>
#include <engine/weather/Residency.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::render {
	inline constexpr uint32_t WEATHER_ASSEMBLY_WORDS = 8;
	inline constexpr uint32_t WEATHER_ASSEMBLY_BYTES = WEATHER_ASSEMBLY_WORDS * sizeof(uint32_t);
	inline constexpr uint32_t WEATHER_ASSEMBLIES_PER_PAGE = weather::BRICK_CELLS / weather::ASSEMBLY_CHILDREN;
	inline constexpr uint32_t WEATHER_COMPRESSED_PAGE_BYTES =
		WEATHER_ASSEMBLIES_PER_PAGE * WEATHER_ASSEMBLY_BYTES;

	struct WeatherPackedAssembly {
		std::array<uint32_t, WEATHER_ASSEMBLY_WORDS> Words{};
		bool operator==(const WeatherPackedAssembly &) const = default;
	};
	static_assert(sizeof(WeatherPackedAssembly) == 32);

	// Scalar bit offsets 0/11/22/33/44: density, biased temperature, vapour, biased pressure, ice.
	// Wind bytes occupy words 2..7 in child X-bit-0/Y-bit-1/Z-bit-2 order, then X/Y/Z per child.
	[[nodiscard]] std::optional<WeatherPackedAssembly>
	PackWeatherAssembly(std::span<const weather::EncodedCell, weather::ASSEMBLY_CHILDREN> children);
	// Quotient/remainder restores exact encoded totals. This is a deliberately lossy scalar shape.
	[[nodiscard]] std::optional<std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN>>
	UnpackWeatherAssembly(const WeatherPackedAssembly &assembly);
	[[nodiscard]] std::array<uint32_t, 2> WeatherAssemblyIndex(uint32_t x, uint32_t y, uint32_t z);
	// All-time density proof includes the coarse and finest reconstruction halo.
	[[nodiscard]] std::optional<bool>
	WeatherPageIsEmpty(const weather::CompiledWeather &compiled, const weather::BrickKey &page);
	// Transient canonical reference samples classify a complete page; no resident CPU field is kept.
	[[nodiscard]] std::optional<bool> WeatherPageCanCompress(
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		const weather::BrickKey &page,
		const weather::AssemblyThresholds &thresholds = {}
	);
}
