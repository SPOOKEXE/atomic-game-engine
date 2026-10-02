#pragma once
#include "SourceRandom.hpp"

#include <optional>
#include <span>
namespace engine::imagegraph::detail {
	struct SourceFlipRandom {
		uint32_t Seed;
		SourceRandom Stream;
		explicit SourceFlipRandom(uint32_t seed) : Seed(seed), Stream(seed) {}
		double SeedRange(double lower, double upper) {
			// Source random_range_seed leaves the ambient stream at seed+1 after one
			// draw.
			SourceRandom first(Seed);
			const double ratio = first.Unit();
			++Seed;
			Stream = SourceRandom(Seed);
			Stream.Unit();
			return lower + (upper - lower) * ratio;
		}
		std::optional<double> Angle(std::span<const double> values, size_t index) {
			const size_t count = values.size();
			if (count == 0) return 0.;
			if (count == 1) return values[0];
			if (count == 2) return SeedRange(values[0], values[1]);
			const auto mode = values[0];
			// Each range in source mode2/3 is evaluated with the same default seed
			// argument.
			const uint32_t seed = Seed;
			auto range = [&](double low, double high) {
				Seed = seed;
				return SeedRange(low, high);
			};
			if (mode == 0) return range(values[1], values[2]);
			if (mode == 1) return range(values[1] - values[2], values[1] + values[2]);
			if ((mode != 2 && mode != 3) || count < 5) return std::nullopt;
			const double type = count > 5 ? values[5] : 0;
			auto a = [&] {
				return mode == 2 ? range(values[1], values[2])
								 : range(values[1] - values[3], values[1] + values[3]);
			};
			auto b = [&] {
				return mode == 2 ? range(values[3], values[4])
								 : range(values[2] - values[3], values[2] + values[3]);
			};
			if (type == 1) return index % 2 ? a() : b();
			if (type != 0) return std::nullopt;
			const double first = a(), second = b();
			const auto slot = size_t(std::floor(Stream.Unit() * 2));
			if (slot >= 2) return std::nullopt;
			return slot == 0 ? first : second;
		}
	};
} // namespace engine::imagegraph::detail
