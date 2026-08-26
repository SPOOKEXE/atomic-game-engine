#include <SDL3/SDL_keycode.h>

#include <client/Actions.hpp>

namespace client {

	namespace {

		struct Binding {
			Action Bound;
			SDL_Keycode Key;
			std::string_view Display;
		};

		// The one table in this program that names a key. Everything else asks
		// for an action, which is what makes rebinding a table edit rather than
		// a search. `input::KeyOf` is the only other place an `SDLK_` appears,
		// and it answers a different question: what a script sees.
		constexpr Binding BINDINGS[] = {
			{Action::Quit, SDLK_ESCAPE, "Esc"},
			{Action::ToggleStatistics, SDLK_F3, "F3"},
			{Action::ToggleNetwork, SDLK_F4, "F4"},
			{Action::ToggleFrameGraph, SDLK_F5, "F5"},
			{Action::NextProfilerTab, SDLK_F6, "F6"},
			{Action::PreviousProfilerTab, SDLK_F7, "F7"},
			{Action::ScrollProfilerUp, SDLK_PAGEUP, "PgUp"},
			{Action::ScrollProfilerDown, SDLK_PAGEDOWN, "PgDn"},
			{Action::DecreaseProfilerDepth, SDLK_MINUS, "-"},
			{Action::IncreaseProfilerDepth, SDLK_EQUALS, "="},
			{Action::WriteProfilerSnapshot, SDLK_F8, "F8"},
			{Action::ToggleWireframe, SDLK_F9, "F9"},
		};
	}

	std::string_view GetActionName(Action action) {
		switch (action) {
		case Action::Quit:
			return "quit";
		case Action::ToggleStatistics:
			return "toggle statistics";
		case Action::ToggleNetwork:
			return "toggle network";
		case Action::ToggleFrameGraph:
			return "toggle frame graph";
		case Action::PreviousProfilerTab:
			return "previous tab";
		case Action::NextProfilerTab:
			return "next tab";
		case Action::ScrollProfilerUp:
			return "scroll up";
		case Action::ScrollProfilerDown:
			return "scroll down";
		case Action::DecreaseProfilerDepth:
			return "shallower graph";
		case Action::IncreaseProfilerDepth:
			return "deeper graph";
		case Action::WriteProfilerSnapshot:
			return "write profiler snapshot";
		case Action::ToggleWireframe:
			return "toggle wireframe";
		case Action::Count:
			break;
		}
		return "?";
	}

	std::string_view GetActionBinding(Action action) {
		for (const auto &binding : BINDINGS) {
			if (binding.Bound == action) {
				return binding.Display;
			}
		}
		return "";
	}

	void Actions::BeginFrame() {
		for (auto &fired : FiredThisFrame) {
			fired = false;
		}
	}

	bool Actions::HandleEvent(const SDL_Event &event) {
		if (event.type == SDL_EVENT_QUIT) {
			FiredThisFrame[static_cast<size_t>(Action::Quit)] = true;
			return true;
		}

		const bool down = event.type == SDL_EVENT_KEY_DOWN;
		const bool up = event.type == SDL_EVENT_KEY_UP;
		if (!down && !up) {
			return false;
		}

		for (const auto &binding : BINDINGS) {
			if (binding.Key != event.key.key) {
				continue;
			}

			// Autorepeat is the OS deciding a held key is many presses. An action
			// is an intent, and holding F5 is one intent.
			if (down && event.key.repeat) {
				return true;
			}

			const auto index = static_cast<size_t>(binding.Bound);
			HeldNow[index] = down;
			if (down) {
				FiredThisFrame[index] = true;
			}
			return true;
		}

		return false;
	}

	bool Actions::Fired(Action action) const {
		return FiredThisFrame[static_cast<size_t>(action)];
	}

	bool Actions::Held(Action action) const {
		return HeldNow[static_cast<size_t>(action)];
	}
}
