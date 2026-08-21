#pragma once

// A text field backed by a `std::string`, and nothing else.
//
// **Here rather than in `mono.studio`, since v0.18.** The editor wrote this and
// `mono.launcher` is the second caller - and it is the widest of the three
// things that moved, because a launcher that mirrors every option is a page of
// text fields. Copying it would have copied the resize-callback dance below,
// which is the part that is wrong in a way that does not look wrong: get the
// order of `resize` and `data()` backwards and imgui writes into freed memory,
// and the corruption surfaces in whatever allocated next.
//
// **Deliberately not the editor's code field.** `studio::CodeField` needs the
// caret out and an insertion in, which is a second callback event and a carrier
// struct that knows about completion. That stays in the editor; this is the
// plain one, which is the one two programs share.
//
// @tier L12 · client

#include <string>

namespace engine::ui {

	// One line of text, edited in place.
	//
	// The string grows as the field does, through imgui's resize callback, so
	// there is no fixed buffer and no truncation at a length nobody chose.
	//
	// @param label  The imgui label, `##`-prefixed to hide it.
	// @param text   The string to edit, in place.
	// @param hint   Placeholder text shown while the field is empty.
	// @param secret Whether to draw the characters as dots. For a shared
	//        secret somebody pastes into a preferences page - it hides the
	//        value from a shoulder or a screen recording and does nothing else,
	//        because the string itself is stored in the clear either way.
	// @return `true` on a frame the text changed.
	// @since v0.7
	bool TextField(const char *label, std::string &text, const char *hint = nullptr, bool secret = false);
}
