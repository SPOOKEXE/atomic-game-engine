#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::render::data_capture_compact {
	// Converts float32 values with IEEE-754 round-to-nearest, ties-to-even. The
	// explicit bit path keeps capture bytes independent of compiler conversion
	// modes. Infinity and NaN keep their IEEE class; finite overflow is rejected.
	inline bool Float32ToFloat16(float value, uint16_t &out) {
		const uint32_t bits = std::bit_cast<uint32_t>(value);
		const uint32_t absolute = bits & 0x7fffffff;
		const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
		if ((absolute >> 23) == 0xff) {
			out = static_cast<uint16_t>(sign | (absolute & 0x7fffff ? 0x7e00 : 0x7c00));
			return true;
		}
		if (absolute > 0x477fe000) return false;
		const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127;
		const uint32_t mantissa = bits & 0x7fffff;
		if (exponent >= -14) {
			uint16_t halfExponent = static_cast<uint16_t>(exponent + 15);
			uint16_t halfMantissa = static_cast<uint16_t>(mantissa >> 13);
			const uint32_t remainder = mantissa & 0x1fff;
			if (remainder > 0x1000 || (remainder == 0x1000 && (halfMantissa & 1) != 0)) {
				if (++halfMantissa == 0x400) {
					halfMantissa = 0;
					++halfExponent;
				}
			}
			out = static_cast<uint16_t>(sign | (halfExponent << 10) | halfMantissa);
			return true;
		}
		// Values with exponent -25 can still round up to the least binary16
		// subnormal. Values below that are strictly below the halfway boundary.
		if (exponent < -25) {
			out = sign;
			return true;
		}
		const uint32_t significant = mantissa | 0x800000;
		const uint32_t shift = static_cast<uint32_t>(-exponent - 1);
		uint16_t halfMantissa = static_cast<uint16_t>(significant >> shift);
		const uint32_t remainder = significant & ((uint32_t{1} << shift) - 1);
		const uint32_t halfway = uint32_t{1} << (shift - 1);
		if (remainder > halfway || (remainder == halfway && (halfMantissa & 1) != 0)) ++halfMantissa;
		out = static_cast<uint16_t>(sign | halfMantissa);
		return true;
	}

	inline float Float16ToFloat32(uint16_t bits);

	inline bool CompactFloat32DepthBytes(
		std::span<const std::byte> source,
		uint32_t width,
		uint32_t height,
		uint32_t sourceRowStride,
		std::vector<std::byte> &out,
		std::optional<double> &maximumError,
		std::string &valueClassification,
		std::string &rejection
	) {
		out.clear();
		maximumError = std::nullopt;
		valueClassification = "not_inspected";
		rejection.clear();
		if (width == 0 || height == 0 || sourceRowStride < width * 4 ||
			source.size() != static_cast<size_t>(sourceRowStride) * height) {
			valueClassification = "unsupported_source_layout";
			rejection = "unsupported_source_layout";
			return false;
		}
		out.resize(static_cast<size_t>(width) * height * 2);
		double finiteMaximumError = 0.0;
		bool hasFinite = false;
		bool hasInfinity = false;
		bool hasNan = false;
		for (size_t row = 0; row < height; ++row)
			for (size_t column = 0; column < width; ++column) {
				const std::byte *bytes = source.data() + row * sourceRowStride + column * 4;
				const uint32_t bits = uint32_t(std::to_integer<uint8_t>(bytes[0])) |
									  uint32_t(std::to_integer<uint8_t>(bytes[1])) << 8 |
									  uint32_t(std::to_integer<uint8_t>(bytes[2])) << 16 |
									  uint32_t(std::to_integer<uint8_t>(bytes[3])) << 24;
				const float value = std::bit_cast<float>(bits);
				const uint32_t absolute = bits & 0x7fffffff;
				hasInfinity = hasInfinity || absolute == 0x7f800000;
				hasNan = hasNan || absolute > 0x7f800000;
				uint16_t half = 0;
				if (!Float32ToFloat16(value, half)) {
					out.clear();
					valueClassification = "finite_overflow";
					rejection = "finite_overflow_above_65504";
					return false;
				}
				if (absolute < 0x7f800000) {
					hasFinite = true;
					const float decoded = Float16ToFloat32(half);
					finiteMaximumError =
						std::max(finiteMaximumError, std::abs(double(value) - double(decoded)));
				}
				const size_t pixel = row * width + column;
				out[pixel * 2] = static_cast<std::byte>(half & 0xff);
				out[pixel * 2 + 1] = static_cast<std::byte>(half >> 8);
			}
		// Error covers the finite samples. Infinity and NaN are classified separately.
		maximumError = hasFinite ? std::optional(finiteMaximumError) : std::nullopt;
		rejection.clear();
		valueClassification = hasInfinity && hasNan ? "contains_infinity_and_nan"
							  : hasInfinity			? "contains_infinity"
							  : hasNan				? "contains_nan"
													: "finite";
		return true;
	}

	inline float Float16ToFloat32(uint16_t bits) {
		const uint32_t sign = uint32_t(bits & 0x8000) << 16;
		uint32_t exponent = (bits >> 10) & 0x1f;
		uint32_t mantissa = bits & 0x3ff;
		if (exponent == 0x1f) return std::bit_cast<float>(sign | 0x7f800000 | (mantissa == 0 ? 0 : 0x400000));
		if (exponent == 0) {
			if (mantissa == 0) return std::bit_cast<float>(sign);
			exponent = 127 - 14;
			while ((mantissa & 0x400) == 0) {
				mantissa <<= 1;
				--exponent;
			}
			mantissa &= 0x3ff;
		} else {
			exponent += 127 - 15;
		}
		return std::bit_cast<float>(sign | (exponent << 23) | (mantissa << 13));
	}
}
