#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>

namespace engine::imagegraph::detail {
	struct SourcePathShiftKey {
		std::array<char, 48> Text{};
		uint8_t Size = 0;
		bool operator==(const SourcePathShiftKey &) const = default;
	};
	// Determine an exact binary64 half-decimal tie before multiplying by the decimal scale.
	inline bool SourceDecimalHalfDown(double value, uint8_t decimals) {
		const uint64_t bits = std::bit_cast<uint64_t>(std::abs(value));
		const uint64_t exponent = (bits >> 52) & 2047;
		const uint64_t mantissa = (bits & ((uint64_t{1} << 52) - 1)) | (exponent ? uint64_t{1} << 52 : 0);
		const int shift = exponent ? int(exponent) - 1023 - 52 + decimals : -1074 + decimals;
		if (shift >= 0 || !mantissa) return false;
		// For two or six decimal places, the rational product needs at most 67 bits.
		uint64_t factor = 1;
		for (uint8_t i = 0; i < decimals; ++i)
			factor *= 5;
		const uint64_t a = (mantissa & 0xffffffffu) * factor;
		const uint64_t b = (mantissa >> 32) * factor;
		const uint64_t low = a + (b << 32);
		const uint64_t high = (b >> 32) + uint64_t(low < a);
		const int bit = -shift - 1;
		if (bit < 64) {
			const bool even = bit == 63 ? (high & 1) == 0 : (low & (uint64_t{1} << (bit + 1))) == 0;
			return even && (low & ((uint64_t{1} << bit) - 1)) == 0 && (low & (uint64_t{1} << bit));
		}
		if (bit >= 128 || low != 0) return false;
		const bool even = bit == 127 || (high & (uint64_t{1} << (bit - 63))) == 0;
		return even && (high & ((uint64_t{1} << (bit - 64)) - 1)) == 0 &&
			   (high & (uint64_t{1} << (bit - 64)));
	}
	inline bool SourceShiftDecimalHalfDown(double value) {
		return SourceDecimalHalfDown(value, 6);
	}
	inline std::optional<SourcePathShiftKey> SourceFixedSampleKey(double ratio, uint8_t decimals) {
		if (!std::isfinite(ratio) || (decimals != 2 && decimals != 6)) return std::nullopt;
		SourcePathShiftKey key;
		if (std::abs(ratio) >= 1e21) {
			// HTML5 toFixed uses the unique shortest round-trip number spelling here.
			// Its equality classes therefore coincide with exact binary64 identity.
			key.Text[0] = 'b';
			const auto end = std::to_chars(
				key.Text.data() + 1, key.Text.data() + key.Text.size(), std::bit_cast<uint64_t>(ratio), 16
			);
			if (end.ec != std::errc{}) return std::nullopt;
			key.Size = uint8_t(end.ptr - key.Text.data());
			return key;
		}
		const auto end = std::to_chars(
			key.Text.data(),
			key.Text.data() + key.Text.size(),
			ratio == 0 ? 0. : ratio,
			std::chars_format::fixed,
			decimals
		);
		if (end.ec != std::errc{}) return std::nullopt;
		key.Size = uint8_t(end.ptr - key.Text.data());
		// C++ rounds a tie to even; JavaScript toFixed rounds its magnitude upward.
		if (SourceDecimalHalfDown(ratio, decimals) && ((key.Text[key.Size - 1] - '0') % 2 == 0)) {
			for (size_t index = key.Size; index; --index) {
				char &digit = key.Text[index - 1];
				if (digit == '.') continue;
				if (digit < '0' || digit > '9') return std::nullopt;
				if (digit != '9') {
					++digit;
					break;
				}
				digit = '0';
			}
		}
		return key;
	}
	inline std::optional<SourcePathShiftKey> SourceShiftRatioKey(double ratio) {
		return SourceFixedSampleKey(ratio, 6);
	}
	inline std::optional<SourcePathShiftKey> SourcePathDistanceKey(double distance) {
		if (!std::isfinite(distance)) return std::nullopt;
		if (distance >= -2147483648. && distance <= 2147483647. && std::trunc(distance) == distance) {
			SourcePathShiftKey key;
			const auto end = std::to_chars(
				key.Text.data(),
				key.Text.data() + key.Text.size(),
				distance == 0 ? 0. : distance,
				std::chars_format::fixed,
				0
			);
			if (end.ec != std::errc{}) return std::nullopt;
			key.Size = uint8_t(end.ptr - key.Text.data());
			return key;
		}
		return SourceFixedSampleKey(distance, 2);
	}
}
