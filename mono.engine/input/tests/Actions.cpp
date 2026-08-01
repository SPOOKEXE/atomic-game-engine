#include <engine/input/Actions.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL_keycode.h>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.input.actions")

using engine::input::Action;
using engine::input::Actions;

namespace {
	SDL_Event KeyEvent(SDL_Keycode key, bool down, bool repeat = false) {
		SDL_Event event {};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.key = key;
		event.key.repeat = repeat;
		return event;
	}
}

TEST_CASE("F3 fires the statistics action", "[input]") {
	Actions actions;
	actions.BeginFrame();

	REQUIRE(actions.HandleEvent(KeyEvent(SDLK_F3, true)));
	REQUIRE(actions.Fired(Action::ToggleStatistics));
	REQUIRE_FALSE(actions.Fired(Action::ToggleFrameGraph));
}

TEST_CASE("F5 fires the frame graph action", "[input]") {
	Actions actions;
	actions.BeginFrame();

	REQUIRE(actions.HandleEvent(KeyEvent(SDLK_F5, true)));
	REQUIRE(actions.Fired(Action::ToggleFrameGraph));
}

TEST_CASE("a fire lasts one frame", "[input]") {
	Actions actions;

	actions.BeginFrame();
	actions.HandleEvent(KeyEvent(SDLK_F5, true));
	REQUIRE(actions.Fired(Action::ToggleFrameGraph));

	actions.BeginFrame();
	REQUIRE_FALSE(actions.Fired(Action::ToggleFrameGraph));
}

TEST_CASE("holding a key does not fire it repeatedly", "[input]") {
	Actions actions;

	actions.BeginFrame();
	actions.HandleEvent(KeyEvent(SDLK_F5, true));
	REQUIRE(actions.Fired(Action::ToggleFrameGraph));

	// The OS turns a held key into a stream of presses. An action is an
	// intent, and holding F5 is one intent.
	actions.BeginFrame();
	actions.HandleEvent(KeyEvent(SDLK_F5, true, /*repeat=*/true));
	REQUIRE_FALSE(actions.Fired(Action::ToggleFrameGraph));
}

TEST_CASE("held tracks the key going down and up", "[input]") {
	Actions actions;

	actions.BeginFrame();
	REQUIRE_FALSE(actions.Held(Action::ScrollProfilerDown));

	actions.HandleEvent(KeyEvent(SDLK_PAGEDOWN, true));
	REQUIRE(actions.Held(Action::ScrollProfilerDown));

	actions.BeginFrame();
	REQUIRE(actions.Held(Action::ScrollProfilerDown));

	actions.HandleEvent(KeyEvent(SDLK_PAGEDOWN, false));
	REQUIRE_FALSE(actions.Held(Action::ScrollProfilerDown));
}

TEST_CASE("an unbound key is not consumed", "[input]") {
	Actions actions;
	actions.BeginFrame();

	REQUIRE_FALSE(actions.HandleEvent(KeyEvent(SDLK_J, true)));
}

TEST_CASE("a window close request is the quit action", "[input]") {
	Actions actions;
	actions.BeginFrame();

	SDL_Event event {};
	event.type = SDL_EVENT_QUIT;

	REQUIRE(actions.HandleEvent(event));
	REQUIRE(actions.Fired(Action::Quit));
}

TEST_CASE("every action has a name and a discoverable binding", "[input]") {
	// The overlay shows the binding, so an action without one is a feature
	// nobody can find.
	for (uint8_t index = 0; index < static_cast<uint8_t>(Action::Count); index++) {
		const auto action = static_cast<Action>(index);
		REQUIRE(engine::input::GetActionName(action) != "?");
		REQUIRE_FALSE(engine::input::GetActionBinding(action).empty());
	}
}
