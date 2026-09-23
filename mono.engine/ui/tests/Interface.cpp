#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Interface.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <string>

TEST_SUITE_ID("engine.ui.interface")

TEST_CASE("headless interface accepts SDL pointer and keyboard events", "[ui][headless]") {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));

	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.x = 120.0f;
	motion.motion.y = 80.0f;
	interface.ProcessEvent(motion);
	SDL_Event click{};
	click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	click.button.button = SDL_BUTTON_LEFT;
	interface.ProcessEvent(click);
	SDL_Event key{};
	key.type = SDL_EVENT_KEY_DOWN;
	key.key.key = SDLK_F5;
	key.key.scancode = SDL_SCANCODE_F5;
	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::GetMousePos().x == 120.0f);
	CHECK(ImGui::GetMousePos().y == 80.0f);
	CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();

	click.type = SDL_EVENT_MOUSE_BUTTON_UP;
	interface.ProcessEvent(click);
	interface.Begin(1.0f / 60.0f);
	CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();

	interface.ProcessEvent(key);
	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::IsKeyDown(ImGuiKey_F5));
	interface.End();

	key.type = SDL_EVENT_KEY_UP;
	interface.ProcessEvent(key);
	interface.Begin(1.0f / 60.0f);
	CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_F5));
	interface.End();

	std::string text = "original";
	SDL_Event typed{};
	typed.type = SDL_EVENT_TEXT_INPUT;
	typed.text.text = text.c_str();
	interface.ProcessEvent(typed);
	text.assign("replaced");
	interface.Begin(1.0f / 60.0f);
	const ImVector<ImWchar> &characters = ImGui::GetIO().InputQueueCharacters;
	REQUIRE(characters.Size >= 8);
	CHECK(characters[0] == 'o');
	CHECK(characters[7] == 'l');
	interface.End();
}

TEST_CASE("automation events retain their pointer position through a frame", "[ui][headless]") {
	engine::render::Renderer renderer;
	engine::ui::Interface interface;
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(interface.Initialise(renderer, nullptr, settings));

	SDL_Event physical{};
	physical.type = SDL_EVENT_MOUSE_MOTION;
	physical.motion.x = 10.0f;
	physical.motion.y = 20.0f;
	interface.ProcessEvent(physical);

	SDL_Event syntheticMotion{};
	syntheticMotion.type = SDL_EVENT_MOUSE_MOTION;
	syntheticMotion.motion.x = 320.0f;
	syntheticMotion.motion.y = 240.0f;
	interface.QueueAutomationEvent(syntheticMotion);
	SDL_Event syntheticDown{};
	syntheticDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	syntheticDown.button.button = SDL_BUTTON_LEFT;
	interface.QueueAutomationEvent(syntheticDown);

	interface.Begin(1.0f / 60.0f);
	CHECK(ImGui::GetMousePos().x == 320.0f);
	CHECK(ImGui::GetMousePos().y == 240.0f);
	CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	interface.End();
}
