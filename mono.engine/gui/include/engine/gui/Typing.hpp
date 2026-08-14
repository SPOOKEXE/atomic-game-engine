#pragma once

// What a keystroke does to the `TextBox` that has the keyboard.
//
// `Input.hpp` decides *which* box the keyboard is going to and this decides what
// arrives there. They are separate because a press and a keystroke are separate
// gestures: the router never sees a key and this never looks at the pointer.
//
// ## The text is the world's, and there is one copy of it
//
// A frame's characters go into `Label::Text` on the focused box and the caret
// into `Entry::CursorPosition` beside it. Nothing here keeps a buffer, an edit
// history or a "string being edited" of its own — rule 2, and the specific bug it
// would buy is a box a script wrote to and a person typed into disagreeing about
// what it says, with the disagreement visible only after the next repaint.
// `Type` is therefore a free function over a store rather than an object with
// state, which is the shape `Layout` and `Focus` already have.
//
// ## A keystroke here is not a platform's
//
// `Typing` names what a keyboard *meant* and no key codes at all. This module
// links `core` and `ecs` and nothing else — `gui/AGENTS.md` refuses `scene`, so
// `scene::KeyCode` is not nameable here and `SDL_Keycode` is three tiers up —
// and the host that owns the pump is the one that knows which key a player
// bound to what. Five meanings is the whole of what a single-line text field
// needs, and a sixth arrives when something can produce it: there is no `Delete`
// and no `Home` here because `scene::KeyCode` has neither, and a field nothing
// can set is a field nothing can test.
//
// ## What it deliberately does not do
//
// **No clipboard and no undo.** Both are a host's, not a widget's: a paste is
// text arriving from somewhere other than a keyboard, which is `Typing::Text`
// already, and an undo stack is state that would have to live somewhere and rule
// 2 says where — a component, once something asks for one.
//
// **No caret drawing.** `DrawKind` has four members and none of them is a line
// blinking at a character offset. `Entry::CursorPosition` is where the answer is
// for whoever draws one.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// One frame's keyboard, in this module's vocabulary.
	//
	// **A state for the frame rather than a stream of keystrokes**, which is
	// `Pointer`'s decision one header along and is made for its reason: a caller
	// polls its device once a frame and hands over what it found, and the order
	// two events arrived in is not part of this module's contract because SDL
	// does not promise one. `Type`'s own comment states the order it applies
	// them in, which is the order a person would have meant.
	//
	// @since v0.15
	struct Typing {
		// What this frame's keystrokes spelled, as UTF-8.
		//
		// **The layout's answer and not the keyboard's**, which is why this is a
		// string and not a set of key codes: `Shift` plus `1` is `!` here, an
		// input method commits a whole word at once, and one character is between
		// one and four bytes. `input::Translator::TypedText` is what produces it
		// on a client.
		std::string_view Text;

		// Whether Backspace was pressed.
		//
		// **Deletes the selection when there is one and one *character* when
		// there is not** — never one byte, which would leave a lone continuation
		// byte behind and turn the rest of the string into a question mark.
		bool Backspace = false;

		// Which way the caret moved, in characters. Negative is left.
		//
		// A count rather than a flag so that a host with key repeat can hand over
		// what a held arrow did, and clamped to the text at both ends.
		int32_t Caret = 0;

		// Whether a caret move extends a selection instead of collapsing one.
		//
		// Shift, on a keyboard. Named for what it does because this struct
		// carries no key codes.
		bool Extend = false;

		// Whether Return was pressed.
		//
		// What it does is `Entry::MultiLine`'s to decide — see `Type`.
		bool Submit = false;
	};

	// What one frame's typing did.
	//
	// @since v0.15
	struct TypeResult {
		// The box it went to, or a null entity when nothing had focus.
		ecs::Entity Instance;

		// Whether `Label::Text` changed.
		//
		// False for a frame that only moved the caret, and false for every frame
		// on a box whose `TextEditable` is off.
		bool Changed = false;

		// Whether Return released the focus.
		//
		// **The caller owes a `FocusReleased` event for this and the router will
		// not produce one**, because no press happened — which is the same split
		// `Input.hpp` states from the other side. `GuiEvent::Entered` is the flag
		// that tells a script this release was Return rather than a click.
		bool Released = false;
	};

	// Applies one frame's keyboard to whichever `TextBox` has the focus.
	//
	// **In one order, and it is the order a person would have meant.** The
	// characters land first, then Backspace, then the caret moves, then Return —
	// so a frame that typed `a` and pressed Backspace ends where it started, and
	// a frame that typed `hi` and pressed Return submits `hi` rather than the text
	// from before it. A frame produces at most one of each of the last three, so
	// there is nothing finer to be true to.
	//
	// **The caret is clamped into the text before anything reads it.** A script
	// may set `TextBox.Text` at any time and the property is a plain field with
	// no setter to hook — see `gui/AGENTS.md` — so a box holding `"hello"` with
	// the caret at 6 can be handed `"hi"` between two frames, and every insertion
	// point derived from that caret would then be past the end. Clamping here
	// rather than at the write is what makes that a no-op instead of a crash, and
	// it is the only place that has to know: this is the one reader that indexes
	// the string by the caret.
	//
	// **Return releases the focus on a single-line box and inserts a line break
	// on a `MultiLine` one.** That is Roblox's rule and the only one that leaves
	// both kinds usable: a search field with no way to say "done" needs a widget
	// added for the purpose, and a multi-line box that submitted on Return could
	// never hold a second line.
	//
	// **`TextEditable` stops the text changing and does not stop the caret
	// moving.** A box a game has locked is still one a person can move through
	// and select in — which is what the property means in Roblox, and what makes
	// a read-only box that a script fills in still legible. Return still releases
	// it, because being done with a box is not an edit.
	//
	// @param store  The world.
	// @param typing What the keyboard did this frame.
	// @return What it did, and to which box.
	// @see gui::Focus
	// @since v0.15
	TypeResult Type(ecs::Store &store, const Typing &typing);
}
