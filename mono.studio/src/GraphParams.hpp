#pragma once

// One ImGui control per ang param, chosen from its widget hint: "toggle",
// "select", "number", "slider" or text for anything else. Shared by the render
// and asset pipeline inspectors.

#include <engine/ang/Registry.hpp>

namespace studio {

	// Draws `param` holding `value`; true when the user changed it this frame.
	// `multiline` gives a text param a multi-line box.
	bool
	DrawGraphParam(const engine::ang::ParamDef &param, engine::ang::Value &value, bool multiline = false);
}
