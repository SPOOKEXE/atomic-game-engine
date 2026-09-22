#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/GuiActions.hpp>

TEST_SUITE_ID("client.gui_actions")

TEST_CASE("keyboard and gamepad edges normalize to the same GUI actions", "[client][gui][input]") {
	engine::scene::InputState keyboard;
	keyboard.Down.Set(engine::scene::KeyCode::Down, true);
	engine::scene::ControllerState controllers;
	controllers.Slots[0].Buttons = 1u << static_cast<uint8_t>(engine::scene::ControllerButton::A);

	const auto actions = client::GuiActions(keyboard, controllers);
	REQUIRE(actions.size() == 2);
	CHECK(actions[0] == engine::gui::SemanticAction::Down);
	CHECK(actions[1] == engine::gui::SemanticAction::Activate);
}

TEST_CASE("text entry owns keyboard navigation and submit keys", "[client][gui][input]") {
	engine::scene::InputState keyboard;
	keyboard.Down.Set(engine::scene::KeyCode::Left, true);
	keyboard.Down.Set(engine::scene::KeyCode::Return, true);
	keyboard.Down.Set(engine::scene::KeyCode::Escape, true);
	engine::scene::ControllerState controllers;

	const auto actions = client::GuiActions(keyboard, controllers, true);
	REQUIRE(actions.size() == 1);
	CHECK(actions.front() == engine::gui::SemanticAction::Cancel);
}
