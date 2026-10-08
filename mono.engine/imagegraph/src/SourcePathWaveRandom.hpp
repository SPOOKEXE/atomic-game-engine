#pragma once

#include "SourceRandom.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace engine::imagegraph::detail {
	// The admitted HTML5 profile converts numbers through signed ~~ before seeding or frac.
	inline std::optional<uint32_t> SourcePathWaveSeedBits(double number) {
		if (!std::isfinite(number)) return {};
		double wrapped = std::fmod(std::trunc(number), 4294967296.);
		if (wrapped < 0) wrapped += 4294967296.;
		return static_cast<uint32_t>(wrapped);
	}

	inline std::optional<double> SourcePathWaveFrac(double number) {
		const auto bits = SourcePathWaveSeedBits(number);
		if (!bits) return {};
		const double fraction = number - std::bit_cast<int32_t>(*bits);
		return std::isfinite(fraction) ? std::optional<double>{fraction} : std::nullopt;
	}

	// Keeps the final reseeded stream for Wave's subsequent iteration draws.
	// SourceRandom::Range has a different draw count and must not replace these helpers.
	class SourcePathWaveRandom {
		SourceRandom Stream{0};

		static std::optional<double> Interpolate(double from, double to, double ratio) {
			const double result = from + (to - from) * ratio;
			return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
		}

	  public:
		bool Seed(double number) {
			const auto bits = SourcePathWaveSeedBits(number);
			if (!bits) return false;
			Stream = SourceRandom(*bits);
			return true;
		}
		double Unit() {
			return Stream.Unit();
		}
		std::optional<double> SeededRange(double from, double to, double seed) {
			if (!std::isfinite(from) || !std::isfinite(to)) return {};
			const auto fraction = SourcePathWaveFrac(seed);
			if (!fraction || !Seed(std::floor(seed))) return {};
			const double first = Unit();
			if (!Seed(std::floor(seed) + 1)) return {};
			const double second = Unit();
			const auto ratio = Interpolate(first, second, *fraction);
			return ratio ? Interpolate(from, to, *ratio) : std::nullopt;
		}
		std::optional<double> Noise(double coordinate) {
			const auto fraction = SourcePathWaveFrac(coordinate);
			if (!fraction || !Seed(std::floor(coordinate))) return {};
			const double first = Unit();
			const double second = Unit();
			const double smooth = *fraction * *fraction * (3. - 2. * *fraction);
			return std::isfinite(smooth) ? Interpolate(first, second, smooth) : std::nullopt;
		}
		std::optional<double> Wiggle(double from, double to, double frequency, double time, double seed) {
			if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(frequency) ||
				!std::isfinite(time) || !std::isfinite(seed))
				return {};
			// Wave calls wiggle with its default one octave, whose amplitude is exactly one.
			const auto noise = Noise(seed + time * frequency);
			return noise ? Interpolate(from, to, *noise) : std::nullopt;
		}
	};
}
