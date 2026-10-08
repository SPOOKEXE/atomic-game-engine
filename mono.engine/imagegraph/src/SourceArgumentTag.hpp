#pragma once

#include "SourcePathShiftKey.hpp"

#include <engine/imagegraph/Document.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace engine::imagegraph::detail {
	struct SourceArgumentTag {
		const std::string *Borrowed = nullptr;
		std::array<char, 48> Text{};
		size_t Size = 0;
		std::string_view View() const {
			return Borrowed ? std::string_view(*Borrowed) : std::string_view(Text.data(), Size);
		}
	};

	// Source struct lookup stringifies raw linked tags using the HTML5 yyGetString profile.
	inline std::optional<SourceArgumentTag> SourceArgumentTagText(const Value &value) {
		SourceArgumentTag tag;
		if (const auto *text = std::get_if<std::string>(&value)) {
			tag.Borrowed = text;
			return tag;
		}
		if (const auto *boolean = std::get_if<bool>(&value)) {
			tag.Text[0] = *boolean ? '1' : '0';
			tag.Size = 1;
			return tag;
		}
		if (const auto *integer = std::get_if<int64_t>(&value)) {
			const auto end = std::to_chars(tag.Text.data(), tag.Text.data() + tag.Text.size(), *integer);
			if (end.ec != std::errc{}) return std::nullopt;
			tag.Size = size_t(end.ptr - tag.Text.data());
			return tag;
		}
		const auto *number = std::get_if<double>(&value);
		if (!number || !std::isfinite(*number)) return std::nullopt;
		if (std::abs(*number) < 1e21) {
			const auto key = SourcePathDistanceKey(*number);
			if (!key) return std::nullopt;
			tag.Text = key->Text;
			tag.Size = key->Size;
			return tag;
		}
		const auto end = std::to_chars(
			tag.Text.data(), tag.Text.data() + tag.Text.size(), *number, std::chars_format::scientific
		);
		if (end.ec != std::errc{}) return std::nullopt;
		tag.Size = size_t(end.ptr - tag.Text.data());
		size_t exponent = 0;
		while (exponent < tag.Size && tag.Text[exponent] != 'e')
			++exponent;
		if (exponent + 2 >= tag.Size || (tag.Text[exponent + 1] != '+' && tag.Text[exponent + 1] != '-'))
			return std::nullopt;
		const size_t firstDigit = exponent + 2;
		size_t significant = firstDigit;
		while (significant + 1 < tag.Size && tag.Text[significant] == '0')
			++significant;
		for (size_t index = significant; index < tag.Size; ++index)
			tag.Text[firstDigit + index - significant] = tag.Text[index];
		tag.Size -= significant - firstDigit;
		return tag;
	}
}
