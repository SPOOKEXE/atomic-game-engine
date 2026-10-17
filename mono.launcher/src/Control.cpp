#include <engine/core/Log.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <launcher/Launcher.hpp>
#include <launcher/Programs.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace launcher {
	using nlohmann::json;

	void Launcher::PumpControl() {
		if (!ControlServer.IsRunning()) return;
		std::vector<engine::control::InputAutomationEvent> releases;
		releases.swap(DeferredControlRelease);
		for (const auto &event : releases)
			DispatchControlInput(event);
		ControlServer.Pump([this](const std::string &line) { return ControlSurface.Answer(line); });
	}

	void Launcher::DispatchControlInput(const engine::control::InputAutomationEvent &input) {
		SDL_Event event{};
		event.common.timestamp = SDL_GetTicksNS();
		const uint32_t window = Window == nullptr ? 0 : SDL_GetWindowID(Window);
		switch (input.Kind) {
		case engine::control::InputAutomationKind::MouseMove:
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = window;
			event.motion.x = input.X;
			event.motion.y = input.Y;
			break;
		case engine::control::InputAutomationKind::MouseButton: {
			event.type = input.State == engine::control::InputAutomationState::Up
							 ? SDL_EVENT_MOUSE_BUTTON_UP
							 : SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.windowID = window;
			event.button.button = input.Button == "right"	 ? SDL_BUTTON_RIGHT
								  : input.Button == "middle" ? SDL_BUTTON_MIDDLE
															 : SDL_BUTTON_LEFT;
			event.button.down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.clicks = 1;
			event.button.x = input.X;
			event.button.y = input.Y;
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = window;
			motion.motion.x = input.X;
			motion.motion.y = input.Y;
			Interface.ProcessEvent(motion);
			break;
		}
		case engine::control::InputAutomationKind::MouseWheel:
			event.type = SDL_EVENT_MOUSE_WHEEL;
			event.wheel.windowID = window;
			event.wheel.y = input.Wheel;
			break;
		case engine::control::InputAutomationKind::Key: {
			event.type = input.State == engine::control::InputAutomationState::Up ? SDL_EVENT_KEY_UP
																				  : SDL_EVENT_KEY_DOWN;
			event.key.windowID = window;
			event.key.key = SDL_GetKeyFromName(input.Key.c_str());
			event.key.scancode = SDL_GetScancodeFromName(input.Key.c_str());
			SDL_Keymod modifiers = SDL_KMOD_NONE;
			for (const std::string &name : input.Modifiers) {
				if (name == "shift") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_SHIFT);
				if (name == "control") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_CTRL);
				if (name == "alt") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_ALT);
				if (name == "gui") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_GUI);
			}
			if (event.key.scancode == SDL_SCANCODE_UNKNOWN)
				event.key.scancode = SDL_GetScancodeFromKey(event.key.key, &modifiers);
			if (event.key.key == SDLK_UNKNOWN)
				event.key.key = SDL_GetKeyFromScancode(event.key.scancode, modifiers, true);
			event.key.mod = event.type == SDL_EVENT_KEY_UP ? SDL_KMOD_NONE : modifiers;
			event.key.down = event.type == SDL_EVENT_KEY_DOWN;
			break;
		}
		case engine::control::InputAutomationKind::Text:
			event.type = SDL_EVENT_TEXT_INPUT;
			event.text.windowID = window;
			event.text.text = input.Text.c_str();
			break;
		}
		Interface.ProcessEvent(event);
	}
}
