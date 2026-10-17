#pragma once

#include <engine/gui/Input.hpp>
#include <engine/scene/Input.hpp>

#include <vector>

namespace client {

	// Normalizes this frame's keyboard and gamepad edges into GUI actions.
	std::vector<engine::gui::SemanticAction> GuiActions(
		const engine::scene::InputState &keyboard,
		const engine::scene::ControllerState &controllers,
		bool textEditing = false
	);
}
