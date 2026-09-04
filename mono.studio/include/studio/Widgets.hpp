#pragma once

// arch-waiver public-header: forward studio API. Editor integrations share
// this complete reusable widget contract.

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
#include <engine/ui/Fields.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace studio {

	// A `core::Name` as a NUL-terminated string, for imgui.
	//
	// **`Name::Text()` returns a `string_view` and imgui takes `const char *`.**
	// The view spans a whole `std::string` held in a deque that never moves and
	// from which nothing is ever removed - `core/src/Name.cpp` says so in as
	// many words - so its `data()` is NUL-terminated and valid for the life of
	// the process. Writing that reasoning out once here beats a `.data()` at
	// forty call sites, each of which is a place somebody later wonders whether
	// it is safe.
	//
	// @param name     The name.
	// @param fallback What to return for an invalid name.
	// @return A NUL-terminated view of the interned text.
	const char *Label(const engine::core::Name &name, const char *fallback = "");

	// **`TextField` is `engine/ui/Fields.hpp`, since v0.18**, re-exported here
	// so that the editor's thirty-two call sites keep reading as the editor's
	// own vocabulary. `mono.launcher` is the second caller and there is one
	// definition; `CodeField` below is the one that stayed, because it needs a
	// caret out and an insertion in and only a script editor wants that.
	using engine::ui::TextField;

	// The caret of a code field, and a change to apply to it.
	//
	// **The seam a completion popup needs, and it is public imgui.** A popup has
	// to know where the caret is to draw beside it and has to be able to replace
	// the word under it to accept. `ScriptEditor.cpp` refuses
	// `ImGuiInputTextState` for exactly this and the refusal stands - that
	// struct is a private *layout* whose fields move between releases, which is
	// why the script editor has Replace All and no Find Next.
	// `ImGuiInputTextCallbackData` is neither private nor a layout: `CursorPos`,
	// `InsertChars` and `DeleteChars` are documented members, and
	// `ImGuiInputTextFlags_CallbackAlways` is how a widget is asked for them.
	//
	// The owner keeps one of these across frames, beside the buffer.
	//
	// @since v0.14
	struct CodeEdit {
		// Where the caret sits, as a byte offset into the text. Only meaningful
		// while `Active`.
		int Caret = 0;

		// Whether the field had keyboard focus this frame. Cleared before the
		// field is submitted and set from inside its callback, so it describes
		// this frame rather than the last one it was true.
		bool Active = false;

		// Text to put in place of the bytes from `ReplaceFrom` up to the caret.
		//
		// **Requested rather than done, because the field owns the edit.**
		// Rewriting the buffer directly would leave imgui's undo stack
		// describing text that is no longer there, so the first Ctrl+Z after
		// accepting a completion would do something nobody asked for. Applied on
		// the next frame and cleared.
		std::string Insert;

		// Where the replacement starts, or -1 for nothing to apply.
		int ReplaceFrom = -1;

		// The active selection, as byte offsets into the text.
		//@{
		int SelectionStart = 0;
		int SelectionEnd = 0;
		//@}
	};

	// A multi-line text field bound to a `std::string`.
	//
	// @param label  The imgui label.
	// @param text   The string to edit, in place.
	// @param edit   The caret to report into and the insertion to apply, or null
	//        to have neither. Passing one costs a callback per frame.
	// @param width  Width in pixels, or 0 to fill.
	// @param height Height in pixels, or 0 to fill.
	// @return `true` on a frame the text changed.
	bool CodeField(
		const char *label,
		std::string &text,
		CodeEdit *edit = nullptr,
		float width = 0.0f,
		float height = 0.0f
	);

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

	// **`FilePrompt` and `FolderPrompt` are `engine/ui/Prompts.hpp`, since
	// v0.18.** They sat here while this editor was their only caller;
	// `mono.launcher` is the second, and a dialog copied into a second program
	// is a second set of keyboard behaviours. `PathPrompt` above stays, because
	// the two dialogs it serves ask for a *name* rather than a path and a
	// folder tree is no help at all with those.

	// A case-insensitive subsequence match, with exact and prefix hits promoted.
	//
	// What the class picker and the property filter both search with. Typing
	// "bp" should find "BasePart", which a substring match does not - and an
	// editor whose search only matches what you already typed correctly is an
	// editor you have to know the answer to use.
	//
	// @param query    What was typed. An empty query matches everything.
	// @param candidate The name being tested.
	// @param score    Filled in with how good the match is, higher being better.
	// @return `true` when every character of `query` appears in order.
	bool FuzzyMatch(std::string_view query, std::string_view candidate, int &score);
}
