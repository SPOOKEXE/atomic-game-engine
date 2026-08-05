#pragma once

// What every key in the editor does, in one table.
//
// **The same shape `input::Actions` gives the client, and for the same
// reason.** That header's first line is "the one table in the engine that names
// a key; everything else asks for an action" — and the editor had drifted from
// it: F5 was spelled out in `DrawShortcuts`, again in the Run menu's label, and
// a third time in a comment. Three copies of a binding are three places to
// change it and two places to forget.
//
// The editor cannot use `input::Actions` itself. That table is SDL keycodes
// read through `input::ActionState`, which is a polled input system a game
// loop drives; the editor's keys are read through imgui, which owns the
// keyboard while a text field has focus and must go on owning it. So this is
// the same idea over `ImGuiKey`, and the two tables are deliberately separate
// — see `Action` for the one binding they disagree about.
//
// **Rebinding is why this exists rather than a `switch`.** A key nobody can
// change is a key that is wrong for somebody, and an editor is a program people
// live in.

#include <cstdint>
#include <filesystem>
#include <imgui.h>
#include <span>
#include <string>

namespace studio {

	// Something the editor can be asked to do with a key.
	//
	// @since v0.7
	enum class Action : uint8_t {
		NewGame,
		OpenGame,
		Save,
		SaveAs,

		Play,
		RunServer,
		Stop,

		Undo,
		Redo,

		Duplicate,
		Delete,
		SelectNone,
		Rename,

		// The two panels the client puts on F3 and F5.
		//
		// **They are unbound here rather than bound to a different pair.** F5 is
		// Play in an editor and always will be, and inventing a second set of
		// function keys for the editor is a decision better made by whoever
		// wants them — which is what the Keybinds page is for.
		ShowStatistics,
		ShowFrameGraph,

		// The palette itself, which is a command like any other — it appears in
		// its own list, and that is correct rather than a curiosity: somebody
		// who has found the palette once should be able to find out what opens
		// it without leaving it.
		CommandPalette,

		Count,
	};

	// Where a binding applies.
	//
	// **A shortcut that fires everywhere is a shortcut that fires in the wrong
	// place.** Delete belongs to the tree and to the viewport, not to the
	// script editor where it is a character; F5 belongs to the transport
	// wherever you are. Without a scope the only guard available is
	// `io.WantCaptureKeyboard`, which answers "is a text field focused" and not
	// "does this key mean anything here" — so a binding on a plain letter could
	// never be added safely at all.
	//
	// @since v0.7
	enum class Scope : uint8_t {
		// Fires wherever the editor has focus. The file and transport commands.
		Global,

		// Only while a viewport is the panel being worked in.
		Viewport,

		// Only while the explorer is. Selection and tree commands.
		Tree,

		// Only while the script editor is, where most keys are text.
		Script,
	};

	// A key and the modifiers held with it.
	//
	// @since v0.7
	struct Chord {
		// The key itself. `ImGuiKey_None` is what an unbound action carries,
		// which is what `IsBound` reads.
		ImGuiKey Key = ImGuiKey_None;

		bool Ctrl = false;   // Whether Ctrl is held with it.
		bool Shift = false;  // Whether Shift is held with it.
		bool Alt = false;    // Whether Alt is held with it.

		// Whether this chord names a key at all.
		//
		// @return `false` for an action nobody has bound.
		bool IsBound() const {
			return Key != ImGuiKey_None;
		}

		// How the chord reads in a menu: `Ctrl+Shift+S`.
		//
		// @return The text, or an empty string when unbound.
		std::string Text() const;

		// Whether two chords are the same key with the same modifiers.
		//
		// @param other The chord to compare against.
		// @return `true` when the key and all three modifiers match.
		bool operator==(const Chord &other) const {
			return Key == other.Key && Ctrl == other.Ctrl && Shift == other.Shift && Alt == other.Alt;
		}
	};

	// One row of the table.
	//
	// @since v0.7
	struct Keybind {
		// The action this row binds. `Action::Count` is the unset value.
		Action Bound = Action::Count;

		// The name in the file, which is not the display name.
		//
		// **A saved binding has to survive an action being renamed for the
		// page, and it has to survive `Action`'s members being reordered** —
		// which an enum value cannot, because the number would then name a
		// different command. This is the only thing written to disk.
		const char *Id = "";

		// What the Keybinds page calls it.
		const char *Name = "";

		// The line that page prints under the name.
		const char *Description = "";

		// Where it applies. See `Scope`.
		Scope Where = Scope::Global;

		// What it is bound to now, which is not necessarily the default.
		Chord Keys;
	};

	// The editor's bindings.
	//
	// **Process-wide, like `input::Actions`.** Two editors in one process would
	// share them, which is a thing a test does and not a thing a person does —
	// and the alternative is threading a binding table through every panel that
	// wants to draw a shortcut in a menu.
	//
	// @since v0.7
	class Keybinds {
	  public:
		// Every binding, in the order the Keybinds page lists them.
		//
		// Mutable, because the page edits them in place.
		//
		// @return The table.
		static std::span<Keybind> All();

		// What an action is bound to.
		//
		// @param action The action.
		// @return Its chord, which may be unbound.
		static Chord Of(Action action);

		// Binds an action, unbinding anything else that held the same chord.
		//
		// **A chord belongs to one action.** Two actions on one key is a key
		// that does two things at once, and the one somebody notices is
		// whichever happens to be checked first — a bug that reads as the
		// editor being haunted.
		//
		// This writes nothing on its own. `Save` is the write, and the editor
		// calls it once on the way out rather than on every rebind — a file
		// rewritten per keystroke of a capture dialog is a file that can be
		// caught half-written.
		//
		// @param action The action to bind.
		// @param chord  The chord, or an unbound one to clear it.
		static void Set(Action action, Chord chord);

		// Puts every binding back to the built-in default.
		static void Reset();

		// Which panel the editor is working in, for this frame.
		//
		// **Set once per frame before anything asks `Fired`.** A binding scoped
		// to the tree must not fire while the pointer is in a viewport, and the
		// only thing that knows which panel is in front is the interface layer.
		//
		// @param scope Where the keyboard currently is.
		static void SetScope(Scope scope);

		// Where the keyboard currently is.
		//
		// @return The scope set for this frame.
		static Scope CurrentScope();

		// Whether any other action holds this chord.
		//
		// **For the page, so a conflict is shown before it is committed rather
		// than discovered as a key that does the wrong thing.** `Set` already
		// unbinds the loser; this is what lets the page say so first.
		//
		// @param chord  The chord to look for.
		// @param ignore An action to skip, normally the row being edited.
		// @return The action holding it, or `Count` when it is free.
		static Action Holder(Chord chord, Action ignore = Action::Count);

		// Reads bindings from a file, leaving unnamed actions alone.
		//
		// **Missing is not an error.** A fresh install has no file and every
		// action keeps its default; an action the file has never heard of keeps
		// its default too, which is what lets a build add a command without
		// invalidating everybody's saved keys.
		//
		// @param path Where to read from.
		// @return `true` when a file was read.
		static bool Load(const std::filesystem::path &path);

		// Writes every bound action to a file.
		//
		// One `id = chord` line each, in table order, so a person can read it
		// and a diff means something. Unbound actions are written too — as an
		// empty chord — because "I cleared this on purpose" has to survive a
		// restart just as a binding does.
		//
		// @param path Where to write.
		// @return `true` when it was written.
		static bool Save(const std::filesystem::path &path);

		// Whether this action's chord was pressed this frame.
		//
		// **Modifiers are checked exactly**, so `Ctrl+S` does not fire on
		// `Ctrl+Shift+S`. Save and Save As were the pair that made this
		// necessary.
		//
		// @param action The action to test.
		// @return `true` on the frame it fired.
		static bool Fired(Action action);

		// The chord being pressed right now, for the rebinding row.
		//
		// Ignores the modifier keys themselves, so holding Ctrl to type
		// `Ctrl+D` does not immediately bind `Ctrl`.
		//
		// @return The chord, or an unbound one when nothing usable is down.
		static Chord Pressed();
	};
}
