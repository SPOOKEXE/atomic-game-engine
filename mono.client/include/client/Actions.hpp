#pragma once

// The client's own diagnostic bindings, and the table that names their keys.
//
// **These are this program's intents and not the engine's**, which is why they
// live here and not in `Engine::input`. Every member below is a panel this
// client draws, a snapshot it writes, the settings overlay, or the request to
// stop. None is a thing a *game* asks for -
// a game reads `scene::InputState`, which `input::Translator` writes, and that
// is the half that genuinely belongs to the engine.
//
// `docs/ARCH_REVIEW.md` C6 named this, and it was right: an engine module at
// L12 whose job is input had thirteen members that only a profiler and a HUD
// could want, and exactly one target in the repository - this one - ever named
// them.
//
// **The key table moved with the enum, deliberately.** `input/AGENTS.md`'s rule
// was "nothing outside this module names a key", and what that rule is actually
// protecting is that a binding lives in *one* table rather than as branches
// spread over a codebase. That property is kept: `BINDINGS` in `Actions.cpp` is
// still the only place in this program where an `SDLK_` appears, and
// `input::KeyOf` is still the only place a keycode becomes a `scene::KeyCode`.
//
// This type translates events. It does not pump them - the program owns its
// event loop, because on some platforms SDL insists on it.
//
// @tier L13 · client
// @since v0.19

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <string_view>

namespace client {

	// An input intent exposed independently of its current key binding.
	//
	// Every action except Count has a human-readable name and binding for UI
	// discovery.
	//
	// @client
	enum class Action : uint8_t {
		// Requests that the client stop. Window close also fires it.
		Quit,
		// Opens or closes the in-game settings menu.
		ToggleSettings,
		// Moves the settings menu selection up.
		SettingsUp,
		// Moves the settings menu selection down.
		SettingsDown,
		// Activates the selected settings menu row.
		SettingsActivate,

		// Toggles the statistics panel.
		ToggleStatistics,
		// Toggles the network panel.
		ToggleNetwork,
		// Toggles the frame graph panel.
		ToggleFrameGraph,
		// Selects the previous profiler tab.
		PreviousProfilerTab,
		// Selects the next profiler tab.
		NextProfilerTab,
		// Scrolls the profiler view up.
		ScrollProfilerUp,
		// Scrolls the profiler view down.
		ScrollProfilerDown,
		// Reduces the visible profiler graph depth.
		DecreaseProfilerDepth,
		// Increases the visible profiler graph depth.
		IncreaseProfilerDepth,
		// Writes the retained profiler history to a snapshot.
		WriteProfilerSnapshot,
		// Toggles wireframe rendering.
		ToggleWireframe,

		// Counts actions and is not itself an action.
		Count,
	};

	// Returns the human-readable name of an action, or "?" for Count or an
	// unknown value.
	//
	// @param action The action to name.
	// @client
	std::string_view GetActionName(Action action);

	// Returns the current binding as display text, or an empty string when no
	// binding exists. UI uses this text to make bindings discoverable.
	//
	// @param action The action whose binding to describe.
	// @client
	std::string_view GetActionBinding(Action action);

	// Translates caller-supplied SDL events into per-frame action state.
	//
	// The caller owns the event pump. This type only consumes individual events
	// and records fired and held states.
	//
	// @client
	class Actions {
	  public:
		// Handles one event and reports whether the action layer consumed it.
		// Window-close requests fire Quit. Bound key transitions update action
		// state; a repeated key-down for a bound action is consumed without firing
		// the action again.
		//
		// @param event The event supplied by the caller-owned SDL event pump.
		// @return True when the action layer consumed the event.
		// @client
		bool HandleEvent(const SDL_Event &event);

		// Starts an input frame by clearing fired edges while preserving held
		// states. Call once before pumping that frame's events, not after them.
		//
		// This method does not pump events.
		//
		// @client
		void BeginFrame();

		// Reports whether the action fired since the last BeginFrame call.
		// This is edge-triggered: a held key fires once, not once per frame.
		//
		// @param action The action to query.
		// @client
		bool Fired(Action action) const;

		// Reports whether the action is currently held.
		// This is level-triggered and remains true across BeginFrame calls until
		// the corresponding key-up event is handled.
		//
		// @param action The action to query.
		// @client
		bool Held(Action action) const;

	  private:
		bool FiredThisFrame[static_cast<size_t>(Action::Count)] = {};
		bool HeldNow[static_cast<size_t>(Action::Count)] = {};
	};
}
