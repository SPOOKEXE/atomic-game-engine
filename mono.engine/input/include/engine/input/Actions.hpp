#pragma once

// Action mapping.
//
// Nothing outside this module names a key. A system asks whether
// `ToggleFrameGraph` fired this frame, and what F5 means is a binding rather
// than a branch spread across the codebase. That is what makes rebinding a
// table edit later instead of a search.
//
// This module translates events. It does not pump them - the program owns its
// event loop, because on some platforms SDL insists on it.
//
// @tier L12 · client

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <string_view>

namespace engine::input {

	// An input intent exposed independently of its current key binding.
	//
	// Every action except Count has a human-readable name and binding for UI
	// discovery.
	//
	// @client
	enum class Action : uint8_t {
		// Requests that the client stop.
		Quit,

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
