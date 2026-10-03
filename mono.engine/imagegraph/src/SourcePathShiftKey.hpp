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
	// Determine an exact binary64 half-decimal tie without rounding value * 1e6.
	inline bool SourceShiftDecimalHalfDown(double value) {
		const uint64_t bits = std::bit_cast<uint64_t>(std::abs(value));
		const uint64_t exponent = (bits >> 52) & 2047;
		const uint64_t mantissa = (bits & ((uint64_t{1} << 52) - 1)) | (exponent ? uint64_t{1} << 52 : 0);
		const int shift = exponent ? int(exponent) - 1023 - 52 + 6 : -1074 + 6;
		if (shift >= 0 || !mantissa) return false;
		// value * 1e6 = mantissa * 15625 * 2^shift. The product needs at most 67 bits.
		const uint64_t a = (mantissa & 0xffffffffu) * 15625;
		const uint64_t b = (mantissa >> 32) * 15625;
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
	inline std::optional<SourcePathShiftKey> SourceShiftRatioKey(double ratio) {
		if (!std::isfinite(ratio)) return std::nullopt;
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
			6
		);
		if (end.ec != std::errc{}) return std::nullopt;
		key.Size = uint8_t(end.ptr - key.Text.data());
		// C++ rounds a tie to even; JavaScript toFixed rounds its magnitude upward.
		if (SourceShiftDecimalHalfDown(ratio) && ((key.Text[key.Size - 1] - '0') % 2 == 0)) {
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
}
