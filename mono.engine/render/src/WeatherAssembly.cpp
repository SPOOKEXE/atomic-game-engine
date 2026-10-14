#include "WeatherAssembly.hpp"

#include <algorithm>
#include <limits>

namespace engine::render {
	std::optional<bool>
	WeatherPageIsEmpty(const weather::CompiledWeather &compiled, const weather::BrickKey &page) {
		if (!weather::ValidBrickKey(page)) return std::nullopt;
		const int64_t minimum = static_cast<int64_t>(page.Origin.Y) - 1;
		const int64_t maximum =
			static_cast<int64_t>(page.Origin.Y) + (static_cast<int64_t>(weather::BRICK_SIDE) << page.Lod) + 1;
		const auto bound = weather::DensityUpperBound(
			compiled.Definition,
			static_cast<int32_t>(std::max(minimum, int64_t{std::numeric_limits<int32_t>::min()})),
			static_cast<int32_t>(std::min(maximum, int64_t{std::numeric_limits<int32_t>::max()}))
		);
		if (!bound) return std::nullopt;
		return *bound == 0;
	}
	std::optional<WeatherPackedAssembly>
	PackWeatherAssembly(std::span<const weather::EncodedCell, weather::ASSEMBLY_CHILDREN> children) {
		WeatherPackedAssembly assembly;
		constexpr std::array<size_t, weather::ASSEMBLY_SCALARS> SCALAR_LANES{0, 1, 2, 3, 7};
		std::array<uint32_t, weather::ASSEMBLY_SCALARS> totals{};
		for (size_t child = 0; child < weather::ASSEMBLY_CHILDREN; ++child) {
			if (!weather::Decode(children[child])) return std::nullopt;
			for (size_t scalar = 0; scalar < totals.size(); ++scalar)
				totals[scalar] += children[child].Codes[SCALAR_LANES[scalar]];
			for (size_t axis = 0; axis < 3; ++axis) {
				const size_t byte = child * 3 + axis;
				assembly.Words[2 + byte / 4] |= static_cast<uint32_t>(children[child].Codes[4 + axis])
												<< ((byte % 4) * 8);
			}
		}
		for (size_t scalar = 0; scalar < totals.size(); ++scalar) {
			const uint32_t offset = static_cast<uint32_t>(scalar) * 11;
			const uint32_t word = offset / 32;
			const uint32_t shift = offset % 32;
			assembly.Words[word] |= totals[scalar] << shift;
			if (shift > 21) assembly.Words[word + 1] |= totals[scalar] >> (32 - shift);
		}
		return assembly;
	}

	std::optional<std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN>>
	UnpackWeatherAssembly(const WeatherPackedAssembly &assembly) {
		// Bits 55..63 are reserved, and checked so future formats cannot silently decode as this one.
		if ((assembly.Words[1] & 0xff800000u) != 0) return std::nullopt;
		constexpr std::array<size_t, weather::ASSEMBLY_SCALARS> SCALAR_LANES{0, 1, 2, 3, 7};
		std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN> children{};
		for (size_t scalar = 0; scalar < SCALAR_LANES.size(); ++scalar) {
			const uint32_t offset = static_cast<uint32_t>(scalar) * 11;
			const uint32_t word = offset / 32;
			const uint32_t shift = offset % 32;
			uint32_t total = assembly.Words[word] >> shift;
			if (shift > 21) total |= assembly.Words[word + 1] << (32 - shift);
			total &= 2047;
			if (total > ((scalar == 1 || scalar == 3) ? 2032u : 2040u)) return std::nullopt;
			for (uint32_t child = 0; child < weather::ASSEMBLY_CHILDREN; ++child)
				children[child].Codes[SCALAR_LANES[scalar]] =
					static_cast<uint8_t>(total / 8 + (child < total % 8 ? 1 : 0));
		}
		for (size_t child = 0; child < weather::ASSEMBLY_CHILDREN; ++child) {
			for (size_t axis = 0; axis < 3; ++axis) {
				const size_t byte = child * 3 + axis;
				children[child].Codes[4 + axis] =
					static_cast<uint8_t>(assembly.Words[2 + byte / 4] >> ((byte % 4) * 8));
			}
			if (!weather::Decode(children[child])) return std::nullopt;
		}
		return children;
	}

	std::array<uint32_t, 2> WeatherAssemblyIndex(uint32_t x, uint32_t y, uint32_t z) {
		return {(x >> 1) + 4 * ((y >> 1) + 4 * (z >> 1)), (x & 1) | ((y & 1) << 1) | ((z & 1) << 2)};
	}

	std::optional<bool> WeatherPageCanCompress(
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		const weather::BrickKey &page,
		const weather::AssemblyThresholds &thresholds
	) {
		if (!weather::ValidBrickKey(page) || time.Revision != compiled.Definition.Revision)
			return std::nullopt;
		if (page.Lod == 0) return false;
		const int32_t stride = int32_t{1} << page.Lod;
		std::array<weather::CellCoordinate, weather::BRICK_CELLS> coordinates{};
		std::array<weather::EncodedCell, weather::BRICK_CELLS> cells{};
		for (uint32_t z = 0; z < 8; ++z)
			for (uint32_t y = 0; y < 8; ++y)
				for (uint32_t x = 0; x < 8; ++x) {
					const uint32_t index = x + 8 * (y + 8 * z);
					coordinates[index] = {
						page.Origin.X + static_cast<int32_t>(x) * stride,
						page.Origin.Y + static_cast<int32_t>(y) * stride,
						page.Origin.Z + static_cast<int32_t>(z) * stride
					};
				}
		if (!weather::EvaluateBatch(compiled.Definition, time, coordinates, cells)) return std::nullopt;
		for (uint32_t z = 0; z < 4; ++z)
			for (uint32_t y = 0; y < 4; ++y)
				for (uint32_t x = 0; x < 4; ++x) {
					std::array<weather::EncodedCell, weather::ASSEMBLY_CHILDREN> children{};
					for (uint32_t child = 0; child < 8; ++child) {
						const uint32_t childX = x * 2 + (child & 1);
						const uint32_t childY = y * 2 + ((child >> 1) & 1);
						const uint32_t childZ = z * 2 + ((child >> 2) & 1);
						children[child] = cells[childX + 8 * (childY + 8 * childZ)];
					}
					const weather::AssemblyRegion region{
						{page.Origin.X + static_cast<int32_t>(x * 2) * stride,
						 page.Origin.Y + static_cast<int32_t>(y * 2) * stride,
						 page.Origin.Z + static_cast<int32_t>(z * 2) * stride},
						static_cast<uint32_t>(stride),
						page.Lod
					};
					const auto summary = weather::Summarize(region, time, children);
					if (!summary) return std::nullopt;
					if (!weather::CanMerge(*summary, compiled.Profile, thresholds)) return false;
				}
		return true;
	}
}
