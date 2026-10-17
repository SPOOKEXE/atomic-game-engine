#pragma once

// The two conversions between a caret and a byte string.
//
// `Entry::CursorPosition` is a one-based grapheme index and `Label::Text` is
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
#include <utf8proc.h>

namespace engine::gui {

	// How many user-visible graphemes a UTF-8 string holds.
	inline size_t Characters(std::string_view text) {
		size_t count = 0;
		utf8proc_int32_t previous = 0;
		utf8proc_int32_t state = 0;
		for (size_t offset = 0; offset < text.size();) {
			utf8proc_int32_t current = 0;
			const utf8proc_ssize_t consumed = utf8proc_iterate(
				reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
				static_cast<utf8proc_ssize_t>(text.size() - offset),
				&current
			);
			if (consumed <= 0) {
				current = 0xFFFD;
				state = 0;
			}
			if (count == 0 || utf8proc_grapheme_break_stateful(previous, current, &state)) count++;
			previous = current;
			offset += consumed > 0 ? static_cast<size_t>(consumed) : 1;
		}
		return count;
	}

	// Where a one-based grapheme position starts, in bytes.
	//
	// **Clamped rather than refused at both ends**, which is what makes a caret
	// left over from longer text harmless: position 1 and anything below it is
	// offset 0, and a position past the last grapheme is `text.size()`.
	inline size_t ByteOffset(std::string_view text, int32_t position) {
		if (position <= 1) {
			return 0;
		}

		int32_t graphemes = 0;
		utf8proc_int32_t previous = 0;
		utf8proc_int32_t state = 0;
		for (size_t offset = 0; offset < text.size();) {
			utf8proc_int32_t current = 0;
			const utf8proc_ssize_t consumed = utf8proc_iterate(
				reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
				static_cast<utf8proc_ssize_t>(text.size() - offset),
				&current
			);
			if (consumed <= 0) {
				current = 0xFFFD;
				state = 0;
			}
			if (graphemes == 0 || utf8proc_grapheme_break_stateful(previous, current, &state)) {
				graphemes++;
				if (graphemes == position) return offset;
			}
			previous = current;
			offset += consumed > 0 ? static_cast<size_t>(consumed) : 1;
		}
		return text.size();
	}
}
