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
#include <vector>
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
	// @param label  The imgui label, `##`-prefixed to hide it.
	// @param text   The string to edit, in place.
	// @param hint   Placeholder text shown while the field is empty.
	// @param secret Whether to draw the characters as dots. For a shared
	//        secret somebody pastes into a preferences page — it hides the
	//        value from a shoulder or a screen recording and does nothing else,
	//        because the string itself is stored in the clear either way.
	// @return `true` on a frame the text changed.
	bool TextField(const char *label, std::string &text, const char *hint = nullptr, bool secret = false);

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

	// A modal that browses for a file, rather than asking for one to be typed.
	//
	// **Beside `PathPrompt` rather than replacing it**, because two of the eight
	// dialogs ask for a *name* — New World and Rename Scene — and a folder tree
	// is no help at all with those. The six that take a path get this; the two
	// that take a name keep the field.
	//
	// Same shape as `PathPrompt` otherwise: opened by `OpenPopup` on the title,
	// returns true on the frame it is confirmed, and leaves `path` holding the
	// answer. See `studio/Browse.hpp` for why this is an imgui browser and not a
	// native dialog.
	//
	// @param title      The popup id, which is also its heading.
	// @param path       The path, in and out. Its folder is where browsing
	//                   starts.
	// @param accept     What the confirm button says.
	// @param extensions Which suffixes to list, lowercase and with the dot.
	//                   Empty lists every file.
	// @param mustExist  Whether the accept button refuses a path that is not
	//                   there. Open refuses; Save As does not, because naming a
	//                   file that does not exist yet is the whole point of it.
	// @return `true` on the frame it is confirmed.
	bool FilePrompt(
		const char *title,
		std::string &path,
		const char *accept,
		const std::vector<std::string> &extensions,
		bool mustExist
	);

	// A modal that browses for a *folder* rather than a file.
	//
	// **Beside `FilePrompt` rather than a flag on it**, because the two differ
	// in more than a filter: a file dialog returns what is selected in the list
	// and a folder dialog returns where the list *is*. Folding them together
	// would mean a click on a row meaning "descend" in one mode and "choose" in
	// the other, from the same widget, which is how a dialog gets a mode nobody
	// can see.
	//
	// Files are listed, greyed, and not selectable — a folder browser that hid
	// them would make it impossible to tell an empty folder from the right one.
	//
	// @param title  The popup id, which is also its heading.
	// @param path   The folder, in and out. Where browsing starts.
	// @param accept What the confirm button says.
	// @return `true` on the frame it is confirmed.
	// @since v0.10
	bool FolderPrompt(const char *title, std::string &path, const char *accept);

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
