#pragma once

// The two conversions between a caret and a byte string.
//
// `Entry::CursorPosition` is a one-based *character* index and `Label::Text` is
// UTF-8 bytes, so every caret operation crosses between the two. Both halves
// live here rather than beside either caller: `gui::Focus` places a caret at the
// end of the text and `gui::Type` inserts at one, and a second copy of this
// arithmetic would be a second answer to where the caret is - one place too far
// for an accented letter and three for an emoji.
//
// Private, because nothing outside this module holds a caret. A public UTF-8
// utility belongs in `core` when something outside asks for one.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine::gui {

	// Whether a byte continues the character before it rather than starting one.
	//
	// **The whole of UTF-8 for this purpose.** A continuation byte is the only
	// one whose top two bits are `10`; everything else - ASCII, and the lead byte
	// of a two-, three- or four-byte sequence - begins a character.
	inline bool Continuation(char byte) {
		return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
	}

	// How many characters a UTF-8 string holds.
	inline size_t Characters(std::string_view text) {
		size_t count = 0;
		for (const char byte : text) {
			count += static_cast<size_t>(!Continuation(byte));
		}
		return count;
	}

	// Where a one-based character position starts, in bytes.
	//
	// **Clamped rather than refused at both ends**, which is what makes a caret
	// left over from longer text harmless: position 1 and anything below it is
	// offset 0, and a position past the last character is `text.size()`. A caller
	// that wanted to know it was out of range compares the characters itself.
	inline size_t ByteOffset(std::string_view text, int32_t position) {
		if (position <= 1) {
			return 0;
		}

		size_t offset = 0;
		int32_t remaining = position - 1;
		while (offset < text.size() && remaining > 0) {
			offset++;
			while (offset < text.size() && Continuation(text[offset])) {
				offset++;
			}
			remaining--;
		}
		return offset;
	}
}
