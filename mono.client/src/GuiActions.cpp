#include <client/GuiActions.hpp>

namespace client {

	std::vector<engine::gui::SemanticAction> GuiActions(
		const engine::scene::InputState &keyboard,
		const engine::scene::ControllerState &controllers,
		bool textEditing
	) {
		std::vector<engine::gui::SemanticAction> actions;
		const auto pressed = [&](engine::scene::KeyCode key) { return keyboard.WasKeyPressed(key); };
		if (!textEditing) {
			if (pressed(engine::scene::KeyCode::Up)) actions.push_back(engine::gui::SemanticAction::Up);
			if (pressed(engine::scene::KeyCode::Down)) actions.push_back(engine::gui::SemanticAction::Down);
			if (pressed(engine::scene::KeyCode::Left)) actions.push_back(engine::gui::SemanticAction::Left);
			if (pressed(engine::scene::KeyCode::Right)) actions.push_back(engine::gui::SemanticAction::Right);
			if (pressed(engine::scene::KeyCode::Return) || pressed(engine::scene::KeyCode::Space))
				actions.push_back(engine::gui::SemanticAction::Activate);
		}
		if (pressed(engine::scene::KeyCode::Escape)) actions.push_back(engine::gui::SemanticAction::Cancel);

		for (const engine::scene::ControllerSlot &controller : controllers.Slots) {
			if (controller.WasPressed(engine::scene::ControllerButton::DPadUp))
				actions.push_back(engine::gui::SemanticAction::Up);
			if (controller.WasPressed(engine::scene::ControllerButton::DPadDown))
				actions.push_back(engine::gui::SemanticAction::Down);
			if (controller.WasPressed(engine::scene::ControllerButton::DPadLeft))
				actions.push_back(engine::gui::SemanticAction::Left);
			if (controller.WasPressed(engine::scene::ControllerButton::DPadRight))
				actions.push_back(engine::gui::SemanticAction::Right);
			if (controller.WasPressed(engine::scene::ControllerButton::A))
				actions.push_back(engine::gui::SemanticAction::Activate);
			if (controller.WasPressed(engine::scene::ControllerButton::B))
				actions.push_back(engine::gui::SemanticAction::Cancel);
		}
		return actions;
	}
}
