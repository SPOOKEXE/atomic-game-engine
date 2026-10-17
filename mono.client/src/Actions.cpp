#include <SDL3/SDL_keycode.h>

#include <client/Actions.hpp>

namespace client {

	namespace {

		struct Binding {
			client::Action Bound;
			SDL_Keycode Key;
			std::string_view Display;
		};

		// The one table in this program that names a key. Everything else asks
		// for an action, which is what makes rebinding a table edit rather than
		// a search. `input::KeyOf` is the only other place an `SDLK_` appears,
		// and it answers a different question: what a script sees.
		constexpr Binding BINDINGS[] = {
			{client::Action::Quit, SDLK_F12, "F12"},
			{client::Action::ToggleSettings, SDLK_ESCAPE, "Esc"},
			{client::Action::SettingsUp, SDLK_UP, "Up"},
			{client::Action::SettingsDown, SDLK_DOWN, "Down"},
			{client::Action::SettingsActivate, SDLK_RETURN, "Enter"},
			{client::Action::ToggleStatistics, SDLK_F3, "F3"},
			{client::Action::ToggleNetwork, SDLK_F4, "F4"},
			{client::Action::ToggleFrameGraph, SDLK_F5, "F5"},
			{client::Action::NextProfilerTab, SDLK_F6, "F6"},
			{client::Action::PreviousProfilerTab, SDLK_F7, "F7"},
			{client::Action::ScrollProfilerUp, SDLK_PAGEUP, "PgUp"},
			{client::Action::ScrollProfilerDown, SDLK_PAGEDOWN, "PgDn"},
			{client::Action::DecreaseProfilerDepth, SDLK_MINUS, "-"},
			{client::Action::IncreaseProfilerDepth, SDLK_EQUALS, "="},
			{client::Action::WriteProfilerSnapshot, SDLK_F8, "F8"},
			{client::Action::ToggleWireframe, SDLK_F9, "F9"},
		};
	}

	std::string_view GetActionName(client::Action action) {
		switch (action) {
		case client::Action::Quit:
			return "quit";
		case client::Action::ToggleSettings:
			return "toggle settings";
		case client::Action::SettingsUp:
			return "settings up";
		case client::Action::SettingsDown:
			return "settings down";
		case client::Action::SettingsActivate:
			return "activate setting";
		case client::Action::ToggleStatistics:
			return "toggle statistics";
		case client::Action::ToggleNetwork:
			return "toggle network";
		case client::Action::ToggleFrameGraph:
			return "toggle frame graph";
		case client::Action::PreviousProfilerTab:
			return "previous tab";
		case client::Action::NextProfilerTab:
			return "next tab";
		case client::Action::ScrollProfilerUp:
			return "scroll up";
		case client::Action::ScrollProfilerDown:
			return "scroll down";
		case client::Action::DecreaseProfilerDepth:
			return "shallower graph";
		case client::Action::IncreaseProfilerDepth:
			return "deeper graph";
		case client::Action::WriteProfilerSnapshot:
			return "write profiler snapshot";
		case client::Action::ToggleWireframe:
			return "toggle wireframe";
		case client::Action::Count:
			break;
		}
		return "?";
	}

	std::string_view GetActionBinding(client::Action action) {
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
			FiredThisFrame[static_cast<size_t>(client::Action::Quit)] = true;
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

	bool Actions::Fired(client::Action action) const {
		return FiredThisFrame[static_cast<size_t>(action)];
	}

	bool Actions::Held(client::Action action) const {
		return HeldNow[static_cast<size_t>(action)];
	}
}
