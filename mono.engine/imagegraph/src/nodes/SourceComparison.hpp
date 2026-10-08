#pragma once

#include "../NodeExecutors.hpp"
#include "../SourceDecimalRange.hpp"
#include "../ValuePayload.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>

// Shared raw getter comparisons for source Condition and Switch. Reference identity needs observations.
namespace engine::imagegraph::detail {
	inline constexpr uint64_t SourceComparisonWorkLimit = 16 * 1024 * 1024;
	struct SourceComparisonKey {
		enum class Kind { Number, Text, Undefined, Identity, Surface } Type = Kind::Undefined;
		double Number = 0;
		std::optional<int64_t> Integer;
		std::string_view Text;
	};
	// Pinned HTML5 yyGetReal/yyCompareVal use this bounded numeric-prefix grammar, not stringification.
	inline std::optional<double> SourceComparisonTextNumber(std::string_view text) {
		constexpr std::array<std::string_view, 11> spaces{
			"\xc2\xa0",
			"\xe1\x9a\x80",
			"\xe2\x80\xa8",
			"\xe2\x80\xa9",
			"\xe2\x80\xaf",
			"\xe2\x81\x9f",
			"\xe3\x80\x80",
			"\xef\xbb\xbf",
			"\t",
			"\n",
			"\r"
		};
		while (!text.empty()) {
			if (text.front() == ' ' || text.front() == '\v' || text.front() == '\f') {
				text.remove_prefix(1);
				continue;
			}
			if (text.size() >= 3 && text.substr(0, 2) == "\xe2\x80" &&
				static_cast<unsigned char>(text[2]) >= 0x80 && static_cast<unsigned char>(text[2]) <= 0x8a) {
				text.remove_prefix(3);
				continue;
			}
			bool removed = false;
			for (auto space : spaces)
				if (text.starts_with(space)) {
					text.remove_prefix(space.size());
					removed = true;
					break;
				}
			if (!removed) break;
		}
		const auto digit = [&](size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
		size_t start = !text.empty() && (text.front() == '+' || text.front() == '-') ? 1 : 0;
		size_t i = start;
		while (i - start < 30 && digit(i))
			++i;
		if (i < text.size() && text[i] == '.' && digit(i + 1)) {
			const auto fraction = ++i;
			while (i - fraction < 30 && digit(i))
				++i;
		} else if (i == start)
			return std::nullopt;
		if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
			auto exponent = i + 1;
			if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) ++exponent;
			if (digit(exponent)) {
				i = exponent + 1;
				if ((text[exponent] == '1' || text[exponent] == '2') && digit(i)) ++i;
			}
		}
		const auto begin = text.data() + (!text.empty() && text.front() == '+' ? 1 : 0);
		double number = 0;
		const auto parsed = std::from_chars(begin, text.data() + i, number);
		if (parsed.ptr != text.data() + i) return std::nullopt;
		if (parsed.ec == std::errc{} && std::isfinite(number)) return number;
		if (parsed.ec == std::errc::result_out_of_range)
			return SourceDecimalRangeUnderflow(text.substr(0, i));
		return std::nullopt;
	}
	inline const Image *SourceComparisonSurface(const NodeContext &c, std::string_view port) {
		for (const auto &[id, image] : c.Images)
			if (id == port) return image;
		return nullptr;
	}
	inline bool SourceComparisonReadKey(
		NodeContext &c, std::string_view port, SourceComparisonKey &key, bool textDefault
	) {
		if (SourceComparisonSurface(c, port)) {
			key.Type = SourceComparisonKey::Kind::Surface;
			return true;
		}
		for (const auto &[id, images] : c.ImageArrays)
			if (id == port && images) {
				key.Type = SourceComparisonKey::Kind::Identity;
				return true;
			}
		const auto *value = c.Find(port);
		if (!value) {
			key.Type = textDefault ? SourceComparisonKey::Kind::Text : SourceComparisonKey::Kind::Number;
			return true;
		}
		if (!ValidRuntimeValue(*value))
			return c.Fail(Status::InvalidValue, "switch selector value is invalid", port);
		if (const auto *text = std::get_if<std::string>(value)) {
			key.Type = SourceComparisonKey::Kind::Text;
			key.Text = *text;
		} else if (std::holds_alternative<UndefinedValue>(*value))
			key.Type = SourceComparisonKey::Kind::Undefined;
		else if (const auto *real = std::get_if<double>(value)) {
			key.Type = SourceComparisonKey::Kind::Number;
			key.Number = *real;
		} else if (const auto *integer = std::get_if<int64_t>(value)) {
			key.Type = SourceComparisonKey::Kind::Number;
			key.Number = double(*integer);
			key.Integer = *integer;
		} else if (const auto *boolean = std::get_if<bool>(value)) {
			key.Type = SourceComparisonKey::Kind::Number;
			key.Number = *boolean ? 1 : 0;
		} else if (const auto *choice = std::get_if<EnumValue>(value)) {
			key.Type = SourceComparisonKey::Kind::Number;
			key.Number = double(choice->Value);
		} else if (const auto *colour = std::get_if<Colour>(value)) {
			key.Type = SourceComparisonKey::Kind::Number;
			key.Number = double(
				uint32_t(colour->Red) | (uint32_t(colour->Green) << 8) | (uint32_t(colour->Blue) << 16) |
				(uint32_t(colour->Alpha) << 24)
			);
		} else if (std::holds_alternative<SurfaceValue>(*value))
			key.Type = SourceComparisonKey::Kind::Surface;
		else
			key.Type = SourceComparisonKey::Kind::Identity;
		return true;
	}
	inline bool SourceComparisonEqual(
		NodeContext &c,
		const SourceComparisonKey &a,
		const SourceComparisonKey &b,
		bool &equal,
		std::string_view port
	) {
		using K = SourceComparisonKey::Kind;
		equal = false;
		if (a.Type == K::Surface || b.Type == K::Surface)
			return c.Fail(
				Status::UnsupportedExecution, "switch surface selector needs source handle observations", port
			);
		if (a.Type == K::Identity && b.Type == K::Identity)
			return c.Fail(
				Status::UnsupportedExecution,
				"switch array or struct selector equality needs source identity observations",
				port
			);
		if (a.Type == K::Identity || b.Type == K::Identity) return true;
		if (a.Type == K::Undefined || b.Type == K::Undefined) {
			equal = a.Type == b.Type;
			return true;
		}
		if (a.Type == K::Text && b.Type == K::Text) {
			equal = a.Text == b.Text;
			return true;
		}
		if (a.Integer && b.Integer) {
			equal = *a.Integer == *b.Integer;
			return true;
		}
		const auto x =
			a.Type == K::Text ? SourceComparisonTextNumber(a.Text) : std::optional<double>(a.Number);
		const auto y =
			b.Type == K::Text ? SourceComparisonTextNumber(b.Text) : std::optional<double>(b.Number);
		equal = x && y && std::abs(*x - *y) <= 1e-5;
		return true;
	}
	struct SourceComparisonUtf16 {
		std::string_view Bytes;
		uint16_t Pending = 0;
		bool Next(uint16_t &unit) {
			if (Pending) {
				unit = Pending;
				Pending = 0;
				return true;
			}
			if (Bytes.empty()) return false;
			const auto first = static_cast<uint8_t>(Bytes.front());
			size_t size = first < 0x80					   ? 1
						  : first >= 0xc2 && first <= 0xdf ? 2
						  : first >= 0xe0 && first <= 0xef ? 3
						  : first >= 0xf0 && first <= 0xf4 ? 4
														   : 0;
			if (!size || Bytes.size() < size) return false;
			uint32_t point = first & (size == 1 ? 0x7f : size == 2 ? 0x1f : size == 3 ? 0xf : 7);
			for (size_t i = 1; i < size; ++i) {
				const auto next = static_cast<uint8_t>(Bytes[i]);
				if ((next & 0xc0) != 0x80) return false;
				point = (point << 6) | (next & 0x3f);
			}
			if ((size == 2 && point < 0x80) || (size == 3 && point < 0x800) ||
				(size == 4 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff)
				return false;
			Bytes.remove_prefix(size);
			if (point <= 0xffff)
				unit = uint16_t(point);
			else {
				point -= 0x10000;
				unit = uint16_t(0xd800 + (point >> 10));
				Pending = uint16_t(0xdc00 + (point & 0x3ff));
			}
			return true;
		}
	};
	// Source strings order UTF-16 code units; UTF-8 bytes reverse astral versus high-BMP ordering.
	inline bool SourceComparisonTextGreaterEqual(
		NodeContext &c, std::string_view a, std::string_view b, bool &matches, std::string_view port
	) {
		for (auto text : {a, b}) {
			SourceComparisonUtf16 check{text};
			uint16_t unit = 0;
			while (check.Next(unit)) {}
			if (!check.Bytes.empty())
				return c.Fail(
					Status::UnsupportedExecution,
					"threshold raw text has no valid Unicode source representation",
					port
				);
		}
		SourceComparisonUtf16 left{a}, right{b};
		uint16_t x = 0, y = 0;
		while (true) {
			const bool hasX = left.Next(x), hasY = right.Next(y);
			if (!hasX || !hasY) {
				matches = hasX || !hasY;
				return true;
			}
			if (x != y) {
				matches = x > y;
				return true;
			}
		}
	}
	inline bool SourceComparisonNumber(
		NodeContext &c, const SourceComparisonKey &key, double &value, std::string_view port
	) {
		using K = SourceComparisonKey::Kind;
		if (key.Type == K::Identity || key.Type == K::Surface)
			return c.Fail(
				Status::UnsupportedExecution,
				"threshold selector getter or array comparison needs source observations",
				port
			);
		if (key.Type == K::Undefined) return false;
		if (key.Type == K::Text) {
			const auto parsed = SourceComparisonTextNumber(key.Text);
			if (!parsed)
				return c.Fail(
					Status::UnsupportedExecution,
					"threshold comparison cannot compare nonnumeric raw text",
					port
				);
			value = *parsed;
		} else
			value = key.Number;
		return true;
	}

	// Pinned Long comparisons subtract first. Overflow needs an observed SDK Long profile.
	inline bool SourceComparisonOrder(
		NodeContext &c,
		const SourceComparisonKey &a,
		const SourceComparisonKey &b,
		std::optional<int> &order,
		std::string_view port
	) {
		using K = SourceComparisonKey::Kind;
		order.reset();
		if (a.Type == K::Identity || b.Type == K::Identity || a.Type == K::Surface || b.Type == K::Surface)
			return c.Fail(
				Status::UnsupportedExecution,
				"source comparison needs observed reference or surface identity",
				port
			);
		if (a.Type == K::Undefined || b.Type == K::Undefined) {
			if (a.Type == b.Type) order = 0;
			return true;
		}
		if (a.Type == K::Text && b.Type == K::Text) {
			bool greaterEqual = false;
			if (!SourceComparisonTextGreaterEqual(c, a.Text, b.Text, greaterEqual, port)) return false;
			order = a.Text == b.Text ? 0 : greaterEqual ? 1 : -1;
			return true;
		}
		if (a.Integer && b.Integer) {
			const int64_t x = *a.Integer, y = *b.Integer;
			if ((y > 0 && x < std::numeric_limits<int64_t>::min() + y) ||
				(y < 0 && x > std::numeric_limits<int64_t>::max() + y))
				return c.Fail(
					Status::UnsupportedExecution,
					"SDK Long overflow comparison requires captured runtime or profile proof",
					port
				);
			const int64_t difference = x - y;
			order = difference == 0 ? 0 : difference < 0 ? -1 : 1;
			return true;
		}
		double x = 0, y = 0;
		if (!SourceComparisonNumber(c, a, x, port) || !SourceComparisonNumber(c, b, y, port))
			return c.FailureCode == Status::Ok;
		order = std::abs(x - y) <= 1e-5 ? 0 : x < y ? -1 : 1;
		return true;
	}
}
