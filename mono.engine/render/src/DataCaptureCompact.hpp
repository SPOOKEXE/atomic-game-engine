#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

	struct RgbNoiseResult {
		std::optional<double> MaximumAbsoluteError;
		std::string ValueClassification = "not_inspected";
		uint64_t EffectiveSigmaQ24 = 0;
		double EffectiveSigma = 0.0;
	};

	inline uint64_t NextNoiseWord(uint64_t &state) {
		// xorshift64* is fixed here rather than using a library distribution, so
		// the stream is stable across standard-library implementations.
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		return state * 2685821657736338717ULL;
	}

	inline uint64_t RoundShiftToEven(uint64_t value, unsigned shift) {
		if (shift == 0) return value;
		if (shift >= 64) return 0;
		const uint64_t quotient = value >> shift;
		const uint64_t remainder = value & ((uint64_t{1} << shift) - 1);
		const uint64_t halfway = uint64_t{1} << (shift - 1);
		return quotient + (remainder > halfway || (remainder == halfway && (quotient & 1) != 0));
	}

	inline uint64_t NoiseState(uint64_t seed) {
		// xorshift's all-zero state is absorbing. Request seed zero maps to this
		// documented nonzero state; every other accepted seed is used directly.
		return seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
	}

	// Twelve bin-centre uniforms are summed in Q17:
	// sum(2 * u_i + 1 - 65536), with u_i in [0, 65535]. Each term has an
	// exact discrete mean of zero, so the CLT approximation has no mean bias.
	inline int64_t GaussianClt12Q17(uint64_t &state) {
		int64_t sum = 0;
		for (size_t index = 0; index < 12; ++index)
			sum += static_cast<int64_t>(2 * (NextNoiseWord(state) >> 48) + 1 - 65536);
		return sum;
	}

	inline bool DoubleToQ24(double value, int64_t &out) {
		const uint64_t bits = std::bit_cast<uint64_t>(value);
		const uint64_t absolute = bits & 0x7fffffffffffffffULL;
		// Both signed zero encodings normalize to the same exact no-noise value.
		if (absolute == 0) {
			out = 0;
			return true;
		}
		if ((bits >> 63) != 0 || (absolute >> 52) == 0x7ff || value < 0.0 || value > 64.0) return false;
		const unsigned exponent = static_cast<unsigned>((absolute >> 52) & 0x7ff);
		const uint64_t significand = exponent == 0
										 ? (absolute & ((uint64_t{1} << 52) - 1))
										 : (uint64_t{1} << 52) | (absolute & ((uint64_t{1} << 52) - 1));
		const int shift = static_cast<int>(exponent) - 1051;
		uint64_t scaled = 0;
		if (shift >= 0) {
			if (shift >= 64 || significand > std::numeric_limits<uint64_t>::max() >> shift) return false;
			scaled = significand << shift;
		} else {
			scaled = RoundShiftToEven(significand, static_cast<unsigned>(-shift));
		}
		out = static_cast<int64_t>(scaled);
		return true;
	}

	inline int64_t Float16ToQ24(uint16_t bits) {
		const int64_t sign = (bits & 0x8000) == 0 ? 1 : -1;
		const uint16_t exponent = static_cast<uint16_t>((bits >> 10) & 0x1f);
		const uint16_t mantissa = static_cast<uint16_t>(bits & 0x3ff);
		const int64_t magnitude = exponent == 0 ? mantissa : int64_t(1024 + mantissa) << (exponent - 1);
		return sign * magnitude;
	}

	inline float Q24ToFloat32(int64_t value) {
		if (value == 0) return 0.0f;
		const bool negative = value < 0;
		uint64_t magnitude = static_cast<uint64_t>(negative ? -value : value);
		const unsigned top = 63u - static_cast<unsigned>(std::countl_zero(magnitude));
		int exponent = static_cast<int>(top) - 24;
		uint64_t significand = top > 23 ? RoundShiftToEven(magnitude, top - 23) : magnitude << (23 - top);
		if (significand == (uint64_t{1} << 24)) {
			significand >>= 1;
			++exponent;
		}
		const uint32_t bits = (negative ? 0x80000000u : 0u) | static_cast<uint32_t>(exponent + 127) << 23 |
							  static_cast<uint32_t>(significand & 0x7fffff);
		return std::bit_cast<float>(bits);
	}

	inline bool ApplyGaussianRgbFloat16(
		std::vector<std::byte> &bytes,
		uint32_t width,
		uint32_t height,
		uint32_t rowStride,
		uint64_t seed,
		double sigma,
		RgbNoiseResult &result
	) {
		result = {};
		result.ValueClassification = "not_inspected";
		constexpr size_t PIXEL_BYTES = 8;
		if (width == 0 || height == 0 || width > std::numeric_limits<uint32_t>::max() / PIXEL_BYTES)
			return false;
		const size_t minimumRowStride = static_cast<size_t>(width) * PIXEL_BYTES;
		if (rowStride < minimumRowStride || height > std::numeric_limits<size_t>::max() / rowStride ||
			bytes.size() != static_cast<size_t>(rowStride) * height)
			return false;
		int64_t sigmaQ24 = 0;
		if (!DoubleToQ24(sigma, sigmaQ24)) return false;
		result.EffectiveSigmaQ24 = static_cast<uint64_t>(sigmaQ24);
		result.EffectiveSigma = double(result.EffectiveSigmaQ24) / double(uint64_t{1} << 24);
		uint64_t state = NoiseState(seed);
		bool hasFinite = false;
		bool hasInfinity = false;
		bool hasNan = false;
		uint64_t maximumErrorQ24 = 0;
		for (size_t row = 0; row < height; ++row) {
			for (size_t column = 0; column < width; ++column) {
				std::byte *pixel = bytes.data() + row * rowStride + column * PIXEL_BYTES;
				for (size_t component = 0; component < 3; ++component) {
					const size_t offset = component * 2;
					const uint16_t source = uint16_t(std::to_integer<uint8_t>(pixel[offset])) |
											uint16_t(std::to_integer<uint8_t>(pixel[offset + 1])) << 8;
					const uint16_t absolute = static_cast<uint16_t>(source & 0x7fff);
					if (absolute > 0x7c00) {
						hasNan = true;
						continue;
					}
					if (absolute == 0x7c00) {
						hasInfinity = true;
						continue;
					}
					hasFinite = true;
					if (sigmaQ24 == 0) continue;
					const int64_t sourceQ24 = Float16ToQ24(source);
					const int64_t sampleQ17 = GaussianClt12Q17(state);
					const int64_t product = sampleQ17 * sigmaQ24;
					const uint64_t magnitude = static_cast<uint64_t>(product < 0 ? -product : product);
					const int64_t deltaQ24 =
						static_cast<int64_t>(RoundShiftToEven(magnitude, 17)) * (product < 0 ? -1 : 1);
					const int64_t limit = int64_t(65504) << 24;
					const int64_t perturbedQ24 = std::clamp(sourceQ24 + deltaQ24, -limit, limit);
					uint16_t stored = 0;
					if (!Float32ToFloat16(Q24ToFloat32(perturbedQ24), stored)) return false;
					pixel[offset] = static_cast<std::byte>(stored & 0xff);
					pixel[offset + 1] = static_cast<std::byte>(stored >> 8);
					const int64_t storedQ24 = Float16ToQ24(stored);
					const uint64_t error = static_cast<uint64_t>(
						storedQ24 >= sourceQ24 ? storedQ24 - sourceQ24 : sourceQ24 - storedQ24
					);
					maximumErrorQ24 = std::max(maximumErrorQ24, error);
				}
			}
		}
		result.MaximumAbsoluteError =
			hasFinite ? std::optional(double(maximumErrorQ24) / double(uint64_t{1} << 24)) : std::nullopt;
		result.ValueClassification = hasInfinity && hasNan ? "contains_infinity_and_nan"
									 : hasInfinity		   ? "contains_infinity"
									 : hasNan			   ? "contains_nan"
														   : "finite";
		return true;
	}

}
