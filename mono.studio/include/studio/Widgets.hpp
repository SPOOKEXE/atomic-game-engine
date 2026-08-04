#pragma once

// The handful of widgets imgui does not ship and this editor needs twice.
//
// **Twice is the bar.** A wrapper around one call used in one place is a layer
// nobody benefits from; a text field that grows a `std::string` is used by the
// script editor, every property of type `Name`, and every path dialog, and
// writing the resize callback three times is three chances to get the capacity
// arithmetic wrong.
//
// Nothing here holds state between frames. A widget that remembered which of
// two editors called it would be a static shared by both, which is a thing a
// test does.

#include <engine/core/Name.hpp>

#include <string>
#include <string_view>

namespace studio {

	// A `core::Name` as a NUL-terminated string, for imgui.
	//
	// **`Name::Text()` returns a `string_view` and imgui takes `const char *`.**
	// The view spans a whole `std::string` held in a deque that never moves and
	// from which nothing is ever removed — `core/src/Name.cpp` says so in as
	// many words — so its `data()` is NUL-terminated and valid for the life of
	// the process. Writing that reasoning out once here beats a `.data()` at
	// forty call sites, each of which is a place somebody later wonders whether
	// it is safe.
	//
	// @param name     The name.
	// @param fallback What to return for an invalid name.
	// @return A NUL-terminated view of the interned text.
	const char *Label(const engine::core::Name &name, const char *fallback = "");

	// A single-line text field bound to a `std::string`.
	//
	// The string grows as the field does, through imgui's resize callback, so
	// there is no fixed buffer and no truncation at a length nobody chose.
	//
	// @param label The imgui label, `##`-prefixed to hide it.
	// @param text  The string to edit, in place.
	// @param hint  Placeholder text shown while the field is empty.
	// @return `true` on a frame the text changed.
	bool TextField(const char *label, std::string &text, const char *hint = nullptr);

	// A multi-line text field bound to a `std::string`.
	//
	// @param label  The imgui label.
	// @param text   The string to edit, in place.
	// @param width  Width in pixels, or 0 to fill.
	// @param height Height in pixels, or 0 to fill.
	// @return `true` on a frame the text changed.
	bool CodeField(const char *label, std::string &text, float width = 0.0f, float height = 0.0f);

	// A toolbar button that reads as pressed while a mode is active.
	//
	// @param label  The caption.
	// @param active Whether the mode it starts is the current one.
	// @param colour The accent to tint it with while active.
	// @return `true` when clicked.
	bool RunButton(const char *label, bool active, unsigned int colour);

	// A modal asking for one string, with Enter to accept and Escape to cancel.
	//
	// **One shape for five questions.** Save As, Open, Import, Export and New
	// World all ask for a path or a name and then do something with it; five
	// hand-written popups would be five places for the Enter key to behave
	// differently, which is exactly the kind of difference nobody notices until
	// it is muscle memory.
	//
	// @param title  The popup's title, and the id `OpenPopup` was given.
	// @param label  What the field is asking for.
	// @param buffer The string being edited, kept by the caller across frames.
	// @param accept The confirming button's caption.
	// @return `true` on the frame the caller should act on `buffer`.
	bool PathPrompt(const char *title, const char *label, std::string &buffer, const char *accept);

	// A case-insensitive subsequence match, with exact and prefix hits promoted.
	//
	// What the class picker and the property filter both search with. Typing
	// "bp" should find "BasePart", which a substring match does not — and an
	// editor whose search only matches what you already typed correctly is an
	// editor you have to know the answer to use.
	//
	// @param query    What was typed. An empty query matches everything.
	// @param candidate The name being tested.
	// @param score    Filled in with how good the match is, higher being better.
	// @return `true` when every character of `query` appears in order.
	bool FuzzyMatch(std::string_view query, std::string_view candidate, int &score);
}
