#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace engine::imagegraph::detail {
	// GameMaker HTML5 Function_Maths.js random_set_seed and irandom, using its default polynomial.
	// Desktop runner equivalence requires separate verification.
	class SourceRandom {
		std::array<uint32_t, 16> State{};
		uint32_t Cursor = 0;

	  public:
		explicit SourceRandom(uint32_t seed) {
			int64_t word = std::bit_cast<int32_t>(seed);
			for (auto &entry : State) {
				const uint32_t wrapped = uint32_t(word * 214013 + 2531011);
				word = uint32_t(std::bit_cast<int32_t>(wrapped) >> 16) & 0x7fffffff;
				entry = uint32_t(word);
			}
		}
		double Unit() {
			uint32_t a = State[Cursor], c = State[(Cursor + 13) & 15];
			const uint32_t b = a ^ c ^ (a << 16) ^ (c << 15);
			c = State[(Cursor + 9) & 15];
			c ^= uint32_t(std::bit_cast<int32_t>(c) >> 11);
			a = State[Cursor] = b ^ c;
			const uint32_t d = a ^ ((a << 5) & 0xDA442D24);
			Cursor = (Cursor + 15) & 15;
			a = State[Cursor];
			State[Cursor] = a ^ b ^ d ^ (a << 2) ^ (b << 18) ^ (c << 28);
			return double(State[Cursor] & 0x7fffffff) / 2147483647.0;
		}
		double Range(double from, double to) {
			if (from == to) return from;
			const double lower = std::min(from, to), higher = std::max(from, to);
			const double result = lower + Unit() * (higher - lower);
			Unit();
			return result;
		}
		// irandom_range consumes one draw even for equal bounds; its final ~~ cast is signed.
		int64_t IntRange(double from, double to) {
			auto int32 = [](double value) {
				if (!std::isfinite(value) || value == 0) return int32_t(0);
				double wrapped = std::fmod(std::trunc(value), 4294967296.0);
				if (wrapped < 0) wrapped += 4294967296.0;
				return std::bit_cast<int32_t>(uint32_t(wrapped));
			};
			const int32_t first = int32(from), second = int32(to);
			const int64_t lower = std::min(first, second), higher = std::max(first, second);
			return lower + int32(Unit() * double(higher - lower + 1));
		}
		uint32_t Index(uint32_t count) {
			if (!count) return 0;
			const uint32_t result = uint32_t(Unit() * count);
			Unit();
			return result;
		}
	};
}
