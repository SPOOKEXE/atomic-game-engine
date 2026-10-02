#pragma once
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
namespace engine::imagegraphexport::detail {
	struct CsvWide {
		uint64_t High = 0, Low = 0;
	};
	inline std::string CsvDecimal(CsvWide value) {
		char buffer[32];
		size_t length = 0;
		do {
			const uint64_t highRemainder = value.High % 10;
			value.High /= 10;
			const uint64_t middle = (highRemainder << 32) | (value.Low >> 32);
			const uint64_t low = ((middle % 10) << 32) | (value.Low & 0xffffffffull);
			value.Low = ((middle / 10) << 32) | (low / 10);
			buffer[length++] = char('0' + low % 10);
		} while (value.High || value.Low);
		std::string result;
		result.reserve(length);
		while (length)
			result += buffer[--length];
		return result;
	}
	// Implements the verified official HTML5 yyGetString number branch without rounding through a product.
	inline std::string CsvNumber(double value) {
		char buffer[32];
		if (value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max() &&
			std::trunc(value) == value) {
			const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), int64_t(value));
			return {buffer, converted.ptr};
		}
		const double magnitude = std::abs(value);
		if (magnitude >= 1e21) {
			const auto converted =
				std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
			return {buffer, converted.ptr};
		}
		const uint64_t bits = std::bit_cast<uint64_t>(magnitude);
		const unsigned exponent = unsigned((bits >> 52) & 2047);
		const uint64_t mantissa = (bits & 0xfffffffffffffull) | (exponent ? 0x10000000000000ull : 0);
		const uint64_t numerator = mantissa * 100;
		const int shift = exponent ? int(exponent) - 1075 : -1074;
		CsvWide cents;
		if (shift >= 0) {
			cents.Low = numerator << shift;
			cents.High = shift ? numerator >> (64 - shift) : 0;
		} else if (shift > -64) {
			const unsigned divisor = unsigned(-shift);
			cents.Low = numerator >> divisor;
			if ((numerator & ((uint64_t(1) << divisor) - 1)) >= (uint64_t(1) << (divisor - 1))) ++cents.Low;
		}
		std::string digits = CsvDecimal(cents);
		if (digits.size() < 3) digits.insert(0, 3 - digits.size(), '0');
		digits.insert(digits.size() - 2, 1, '.');
		if (value < 0) digits.insert(0, 1, '-');
		return digits;
	}
}
