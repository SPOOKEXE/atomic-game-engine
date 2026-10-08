#pragma once

#include "SourceDecimalRange.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace engine::imagegraph::detail {
	// grug admitted HTML5 real text profile. Invalid text is zero; nonfinite conversion is unsupported.
	inline std::optional<double> SourceRealTextNumber(std::string_view input) {
		if (input.size() > Limits::MaximumTextBytes) return {};
		if (input.starts_with("0x")) {
			input.remove_prefix(2);
			size_t count = 0;
			for (char digit : input) {
				if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
					  (digit >= 'A' && digit <= 'F')))
					break;
				++count;
			}
			if (!count) return 0.;
			// parseInt rounds the whole integer once. Repeated digit arithmetic loses low rounding bits.
			// Limit the hex grammar to its integer prefix, excluding dots and binary exponents.
			double number = 0;
			const auto parsed =
				std::from_chars(input.data(), input.data() + count, number, std::chars_format::hex);
			return parsed.ec == std::errc{} && parsed.ptr == input.data() + count && std::isfinite(number)
					   ? std::optional<double>{number}
					   : std::nullopt;
		}
		// grug match parseFloat leading whitespace without a locale or a Unicode database.
		constexpr std::array<std::string_view, 19> unicodeSpaces{
			"\xc2\xa0",
			"\xe1\x9a\x80",
			"\xe2\x80\x80",
			"\xe2\x80\x81",
			"\xe2\x80\x82",
			"\xe2\x80\x83",
			"\xe2\x80\x84",
			"\xe2\x80\x85",
			"\xe2\x80\x86",
			"\xe2\x80\x87",
			"\xe2\x80\x88",
			"\xe2\x80\x89",
			"\xe2\x80\x8a",
			"\xe2\x80\xa8",
			"\xe2\x80\xa9",
			"\xe2\x80\xaf",
			"\xe2\x81\x9f",
			"\xe3\x80\x80",
			"\xef\xbb\xbf"
		};
		while (!input.empty()) {
			if (input.front() == ' ' || (input.front() >= '\t' && input.front() <= '\r')) {
				input.remove_prefix(1);
				continue;
			}
			const auto space = std::find_if(unicodeSpaces.begin(), unicodeSpaces.end(), [&](auto prefix) {
				return input.starts_with(prefix);
			});
			if (space == unicodeSpaces.end()) break;
			input.remove_prefix(space->size());
		}
		if (input.starts_with('+')) {
			input.remove_prefix(1);
			if (input.starts_with('-')) return 0.;
		}
		if (input.starts_with("Infinity") || input.starts_with("-Infinity")) return {};
		const auto numeric = input.starts_with('-') ? input.substr(1) : input;
		if (numeric.empty() || ((numeric.front() < '0' || numeric.front() > '9') && numeric.front() != '.'))
			return 0.;
		double number = 0;
		const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
		if (parsed.ec == std::errc::result_out_of_range) {
			const std::string_view prefix{input.data(), size_t(parsed.ptr - input.data())};
			if (const auto underflow = SourceDecimalRangeUnderflow(prefix)) return *underflow;
			return {};
		}
		if (parsed.ec == std::errc::invalid_argument) return 0.;
		return std::isfinite(number) ? std::optional<double>{number} : std::nullopt;
	}
	// grug primitive toNumber keeps boolean and exact integer carriers before trying real on text.
	inline std::optional<Value> SourceRealNumber(const Value &raw) {
		if (std::holds_alternative<bool>(raw) || std::holds_alternative<int64_t>(raw) ||
			std::holds_alternative<double>(raw))
			return raw;
		if (std::holds_alternative<ArrayValue>(raw) || std::holds_alternative<UndefinedValue>(raw))
			return Value{0.};
		const auto *text = std::get_if<std::string>(&raw);
		if (!text) return {};
		const auto number = SourceRealTextNumber(*text);
		return number ? std::optional<Value>{Value{*number}} : std::nullopt;
	}
}
