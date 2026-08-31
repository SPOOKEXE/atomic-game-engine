#pragma once

#include <engine/input/Translate.hpp>

#include <vector>

struct SDL_Gamepad;
struct SDL_Joystick;

namespace studio {

	// SDL device ownership and translation for Studio Play. Kept behind Editor's
	// private boundary so the already-large public Editor type does not expose
	// platform input collaborators.
	struct PlayedInputAdapter {
		engine::input::Translator Translator;
		std::vector<SDL_Gamepad *> Gamepads;
		std::vector<SDL_Joystick *> Joysticks;
	};
}
