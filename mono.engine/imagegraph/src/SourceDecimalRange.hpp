#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <optional>
#include <string_view>

namespace engine::imagegraph::detail {
	// grug call only after decimal from_chars range failure. Below one is underflow, above one overflow.
	inline std::optional<double> SourceDecimalRangeUnderflow(std::string_view prefix) {
		if (prefix.empty() || prefix.size() > Limits::MaximumTextBytes) return {};
		const bool negative = prefix.front() == '-';
		if (negative || prefix.front() == '+') prefix.remove_prefix(1);
		int64_t digits = 0, integerDigits = 0;
		std::optional<int64_t> firstNonzero;
		bool fractional = false;
		size_t position = 0;
		for (; position < prefix.size(); ++position) {
			const char digit = prefix[position];
			if (digit == 'e' || digit == 'E') break;
			if (digit == '.' && !fractional) {
				fractional = true;
				continue;
			}
			if (digit < '0' || digit > '9') return {};
			if (!firstNonzero && digit != '0') firstNonzero = digits;
			++digits;
			if (!fractional) ++integerDigits;
		}
		if (!digits) return {};
		int64_t exponent = 0;
		bool negativeExponent = false;
		if (position < prefix.size()) {
			++position;
			if (position < prefix.size() && (prefix[position] == '-' || prefix[position] == '+')) {
				negativeExponent = prefix[position] == '-';
				++position;
			}
			if (position == prefix.size()) return {};
			// grug saturation exceeds every admitted mantissa offset, so huge exponents cannot overflow.
			constexpr int64_t cap = int64_t(Limits::MaximumTextBytes) + 1024;
			for (; position < prefix.size(); ++position) {
				const char digit = prefix[position];
				if (digit < '0' || digit > '9') return {};
				exponent = std::min(cap, exponent * 10 + (digit - '0'));
			}
		}
		if (!firstNonzero) return negative ? -0. : 0.;
		if (negativeExponent) exponent = -exponent;
		if (integerDigits - *firstNonzero - 1 + exponent < 0) return negative ? -0. : 0.;
		return {};
	}
}
