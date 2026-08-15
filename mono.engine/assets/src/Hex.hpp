#pragma once

// Hex, for the fixed-size byte arrays this module names things with.
//
// A content hash, a public key and a signature are the same shape - N bytes
// written as 2N characters - and each is parsed back from text that arrived
// from somewhere. One codec, so there is one spelling of an address and one
// place that refuses the others.
//
// Private to this module. Nothing outside `assets` writes a content address,
// and a general-purpose hex utility at a lower tier would invite it to.
//
// @tier L8 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::assets {

	constexpr char HEX_DIGITS[] = "0123456789abcdef";

	// -1 for anything that is not a lowercase hex digit. Uppercase is refused
	// along with everything else: one spelling per address.
	constexpr int HexValue(char character) {
		if (character >= '0' && character <= '9') {
			return character - '0';
		}
		if (character >= 'a' && character <= 'f') {
			return character - 'a' + 10;
		}
		return -1;
	}

	// Lowercase hex, two characters per byte.
	//
	// @param value The bytes to write out.
	// @return Exactly `Length * 2` characters.
	template <size_t Length> std::string ToHexString(const std::array<uint8_t, Length> &value) {
		std::string text(Length * 2, '\0');
		for (size_t index = 0; index < Length; ++index) {
			text[index * 2] = HEX_DIGITS[value[index] >> 4];
			text[index * 2 + 1] = HEX_DIGITS[value[index] & 0x0F];
		}
		return text;
	}

	// Parses what ToHexString wrote, and nothing else.
	//
	// A wrong length, a `0x` prefix and uppercase are all refused, because two
	// spellings of one address are two cache keys for one piece of content.
	//
	// @param[in]  text  Exactly `Length * 2` lowercase hex characters.
	// @param[out] value The bytes. Written as the text is walked, so a failure
	//                   part-way leaves some of them set - parse into a local
	//                   and only publish it once this has returned true.
	// @return Whether the whole text parsed.
	template <size_t Length> bool FromHexString(std::string_view text, std::array<uint8_t, Length> &value) {
		if (text.size() != Length * 2) {
			return false;
		}
		for (size_t index = 0; index < Length; ++index) {
			const int high = HexValue(text[index * 2]);
			const int low = HexValue(text[index * 2 + 1]);
			if (high < 0 || low < 0) {
				return false;
			}
			value[index] = static_cast<uint8_t>((high << 4) | low);
		}
		return true;
	}
}
