#pragma once

// SDL events into `scene::InputState`, which is the only place SDL keycodes are
// turned into names.
//
// **Here rather than in the client, because `Actions.hpp` already says the
// rule**: nothing outside this module names a key. `Actions` is the half that
// maps a key to an *intent* — F5 toggles the frame graph — and this is the half
// that reports what is held, which is what a game's own scripts need. Two halves
// of one question, and both belong to whichever module owns the keyboard.
//
// **It writes a `shared` type and reads a `client` one**, which is the whole
// point: `scene::InputState` is a resource a script can reach and an SDL event is
// not, and this is the one function that crosses between them.
//
// This module still pumps nothing. The caller owns the event loop — SDL insists
// on it on some platforms — and hands events here one at a time, exactly as it
// hands them to `Actions`.
//
// @tier L12 · client

#include <engine/scene/Input.hpp>

#include <SDL3/SDL_events.h>

namespace engine::input {

	// Accumulates SDL events into one frame's input state.
	//
	// **A class rather than a free function, because a frame has a shape**: the
	// deltas accumulate across however many events arrive and then have to be
	// cleared, and the previous frame's key bits have to be kept to produce the
	// edges. A free function taking one event could do neither.
	//
	// @since v0.10
	// @client
	class Translator {
	  public:
		// Starts a frame: rolls `Down` into `Previous` and clears the deltas.
		//
		// **Call once before pumping that frame's events, not after them** —
		// `Actions::BeginFrame`'s rule and for the same reason: a delta cleared
		// after the pump is a delta nobody ever read.
		void BeginFrame();

		// Folds one event into the state.
		//
		// Ignores anything that is not a key, a button, a motion, a wheel or a
		// focus change. **Returns whether it was consumed rather than swallowing
		// it**, because the caller also feeds `Actions` and an editor's interface
		// layer, and each of them decides for itself.
		//
		// @param event The event from the caller-owned pump.
		// @return `true` when this event changed the state.
		bool HandleEvent(const SDL_Event &event);

		// The state as of the events handled so far.
		//
		// @return The state, to be copied onto the world's resource.
		const scene::InputState &State() const {
			return Current;
		}

		// Clears every key and button, keeping the focus flag.
		//
		// **What losing focus does.** Alt-tabbing away while holding W must not
		// leave a character walking forever — SDL sends no key-up for a key
		// released in another window, so the release has to be manufactured here.
		// `scene::InputState::Focused` records why, and the character controller
		// checks it as a second belt.
		void ReleaseAll();

	  private:
		scene::InputState Current;
	};

	// The key an SDL keycode names, or `Unknown`.
	//
	// **Exported so a test can check the table without an event loop**, which is
	// the only way this mapping gets checked at all: it is ninety-odd entries, and
	// one wrong line is a key that silently does nothing.
	//
	// @param code The SDL keycode.
	// @return The named key.
	scene::KeyCode KeyOf(SDL_Keycode code);
}
