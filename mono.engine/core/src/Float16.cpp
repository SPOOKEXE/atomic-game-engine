#include <engine/core/Float16.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace engine::core {
	namespace {
		uint32_t RoundShift(uint32_t value, uint32_t shift) noexcept {
			const uint32_t truncated = value >> shift;
			const uint32_t remainder = value & ((uint32_t{1} << shift) - 1);
			const uint32_t halfway = uint32_t{1} << (shift - 1);
			return truncated + (remainder > halfway || (remainder == halfway && (truncated & 1)));
		}
	} // namespace

	uint16_t EncodeFloat16(float value) noexcept {
		const uint32_t bits = std::bit_cast<uint32_t>(value);
		const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
		const uint32_t exponent = (bits >> 23) & 255, fraction = bits & 0x7fffff;
		if (exponent == 255) return sign | 0x7c00 | (fraction ? 0x0200 : 0);
		const int32_t adjusted = int32_t(exponent) - 127 + 15;
		if (adjusted >= 31) return sign | 0x7c00;
		if (adjusted <= 0) {
			if (adjusted < -10) return sign;
			return sign | uint16_t(RoundShift(fraction | 0x800000, uint32_t(14 - adjusted)));
		}
		return sign | uint16_t((uint32_t(adjusted) << 10) + RoundShift(fraction, 13));
	}

	float DecodeFloat16(uint16_t bits) noexcept {
		const bool negative = (bits & 0x8000) != 0;
		const uint32_t exponent = (bits >> 10) & 31, fraction = bits & 1023;
		float result;
		if (exponent == 31)
			result =
				fraction ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
		else if (exponent == 0)
			result = std::ldexp(float(fraction), -24);
		else
			result = std::ldexp(float(1024 + fraction), int(exponent) - 25);
		return negative ? -result : result;
	}
} // namespace engine::core
