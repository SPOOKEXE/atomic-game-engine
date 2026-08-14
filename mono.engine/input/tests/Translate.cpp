// SDL events into `scene::InputState`, checked without an event loop.
//
// **The suite `Translate.hpp` did not have.** Every other public header in the
// engine is covered by one and this was the gap: the keycode table is ninety-odd
// entries where one wrong line is a key that silently does nothing, and the
// frame shape — roll the four `Previous` fields, clear the two deltas — is the
// half every edge a script sees is derived from.
//
// A `Translator` needs no window and no SDL subsystem: `HandleEvent` takes a
// struct, which is exactly why the caller owns the pump.

#include <engine/input/Translate.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL_events.h>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.input.translate")
TEST_DEPENDS("engine.scene.input")

using engine::input::KeyOf;
using engine::input::Translator;
using engine::scene::InputSource;
using engine::scene::KeyCode;
using engine::scene::MouseButton;

namespace {
	SDL_Event KeyEvent(SDL_Keycode key, bool down, bool repeat = false) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.key = key;
		event.key.repeat = repeat ? 1 : 0;
		return event;
	}

	// One `SDL_EVENT_TEXT_INPUT`, carrying whatever UTF-8 a keystroke produced.
	//
	// **The pointer is into the caller's literal, which is what SDL does too.**
	// `SDL_TextInputEvent::text` is borrowed for the duration of the handler and
	// the translator copies out of it — a test that owned the storage would be
	// testing a contract the platform does not offer.
	SDL_Event TextEvent(const char *text) {
		SDL_Event event{};
		event.type = SDL_EVENT_TEXT_INPUT;
		event.text.text = text;
		return event;
	}

	SDL_Event ButtonEvent(uint8_t button, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.button = button;
		return event;
	}

	SDL_Event MotionEvent(float x, float y, float dx, float dy) {
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.x = x;
		event.motion.y = y;
		event.motion.xrel = dx;
		event.motion.yrel = dy;
		return event;
	}

	SDL_Event WheelEvent(float notches) {
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_WHEEL;
		event.wheel.y = notches;
		return event;
	}

	SDL_Event WindowEvent(bool focused) {
		SDL_Event event{};
		event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
		return event;
	}
}

TEST_CASE("the keycode table round-trips every named key", "[input]") {
	// **The whole table rather than a sample**, because one wrong line is one key
	// that does nothing and a sample is how it survives. Every key SDL names in
	// the dense ASCII run plus the four groups that are not dense.
	struct Mapping {
		SDL_Keycode From;
		KeyCode To;
	};

	const Mapping MAPPINGS[] = {
		{SDLK_A, KeyCode::A},
		{SDLK_Z, KeyCode::Z},
		{SDLK_0, KeyCode::Zero},
		{SDLK_9, KeyCode::Nine},
		{SDLK_SPACE, KeyCode::Space},
		{SDLK_RETURN, KeyCode::Return},
		{SDLK_ESCAPE, KeyCode::Escape},
		{SDLK_BACKSPACE, KeyCode::Backspace},
		{SDLK_TAB, KeyCode::Tab},
		{SDLK_LSHIFT, KeyCode::LeftShift},
		{SDLK_RSHIFT, KeyCode::RightShift},
		{SDLK_LCTRL, KeyCode::LeftControl},
		{SDLK_RCTRL, KeyCode::RightControl},
		{SDLK_LALT, KeyCode::LeftAlt},
		{SDLK_RALT, KeyCode::RightAlt},
		{SDLK_UP, KeyCode::Up},
		{SDLK_DOWN, KeyCode::Down},
		{SDLK_LEFT, KeyCode::Left},
		{SDLK_RIGHT, KeyCode::Right},
		{SDLK_F1, KeyCode::F1},
		{SDLK_F12, KeyCode::F12},
	};

	for (const Mapping &mapping : MAPPINGS) {
		INFO(engine::scene::Describe(mapping.To));
		CHECK(KeyOf(mapping.From) == mapping.To);
	}

	// **A key this engine has no name for is `Unknown` rather than something
	// nearby**, which is what keeps `Enum.KeyCode` honest: a member that mapped
	// to nothing would be offering an author completion for a key that never
	// fires.
	CHECK(KeyOf(SDLK_HOME) == KeyCode::Unknown);
	CHECK(KeyOf(SDLK_F13) == KeyCode::Unknown);
}

TEST_CASE("a frame's edges are the difference between two frames", "[input]") {
	Translator translator;

	// **Two frames and the state read in both, not one frame and a flag.** The
	// bug this shape catches is a `Previous` that is rolled at the wrong end of
	// the frame: a press then reads as an edge for ever, or never reads as one at
	// all, and a single frame cannot tell the two apart.
	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_W, true)));

	CHECK(translator.State().IsKeyDown(KeyCode::W));
	CHECK(translator.State().WasKeyPressed(KeyCode::W));
	CHECK_FALSE(translator.State().WasKeyReleased(KeyCode::W));

	// Held. Down, and no longer an edge — which is what makes `InputBegan` fire
	// once rather than every frame a key is held.
	translator.BeginFrame();
	CHECK(translator.State().IsKeyDown(KeyCode::W));
	CHECK_FALSE(translator.State().WasKeyPressed(KeyCode::W));

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_W, false)));
	CHECK_FALSE(translator.State().IsKeyDown(KeyCode::W));
	CHECK(translator.State().WasKeyReleased(KeyCode::W));

	// **A repeat is refused rather than folded in.** SDL sends key-down again
	// while a key is held; an edge derived from anything but the two bitsets
	// would fire once per repeat.
	translator.BeginFrame();
	CHECK_FALSE(translator.HandleEvent(KeyEvent(SDLK_W, true, true)));
	CHECK_FALSE(translator.State().IsKeyDown(KeyCode::W));
}

TEST_CASE("the pointer accumulates within a frame and starts each one at zero", "[input]") {
	Translator translator;

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(MotionEvent(10.0f, 20.0f, 3.0f, 4.0f)));
	REQUIRE(translator.HandleEvent(MotionEvent(14.0f, 26.0f, 4.0f, 6.0f)));
	REQUIRE(translator.HandleEvent(WheelEvent(1.0f)));
	REQUIRE(translator.HandleEvent(WheelEvent(2.0f)));

	// **Summed and not assigned**, because several motion events arrive per frame
	// and a camera wants all of the movement — assigning would make the turn
	// depend on how the compositor happened to batch.
	CHECK(translator.State().MousePosition.X == 14.0f);
	CHECK(translator.State().MouseDelta.X == 7.0f);
	CHECK(translator.State().MouseDelta.Y == 10.0f);
	CHECK(translator.State().WheelDelta == 3.0f);

	// **The position survives the frame and the deltas do not**, which is the
	// whole of why `BeginFrame` clears two fields and rolls three others: a
	// pointer that is not moving is still somewhere.
	translator.BeginFrame();
	CHECK(translator.State().MousePosition.X == 14.0f);
	CHECK(translator.State().MouseDelta.X == 0.0f);
	CHECK(translator.State().WheelDelta == 0.0f);

	// The buttons are edges in their own space, and a button this engine has no
	// name for is refused rather than folded into one it does.
	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(ButtonEvent(SDL_BUTTON_RIGHT, true)));
	CHECK(translator.State().IsButtonDown(MouseButton::Right));
	CHECK(translator.State().WasButtonPressed(MouseButton::Right));
	CHECK_FALSE(translator.State().IsButtonDown(MouseButton::Left));
	CHECK_FALSE(translator.HandleEvent(ButtonEvent(SDL_BUTTON_X1, true)));
}

TEST_CASE("losing focus releases everything and says so", "[input]") {
	Translator translator;

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_W, true)));
	REQUIRE(translator.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true)));

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(WindowEvent(false)));

	// **Alt-tabbing while holding W must not leave a character walking**, and SDL
	// sends no key-up for a key let go in another window — so the release is
	// manufactured here and has to read as a release rather than as never having
	// happened. `Previous` is what makes the difference, so both halves are
	// checked.
	CHECK_FALSE(translator.State().Focused);
	CHECK(translator.State().WasFocusLost());
	CHECK_FALSE(translator.State().IsKeyDown(KeyCode::W));
	CHECK(translator.State().WasKeyReleased(KeyCode::W));
	CHECK(translator.State().WasButtonReleased(MouseButton::Left));

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(WindowEvent(true)));
	CHECK(translator.State().WasFocusGained());
	CHECK_FALSE(translator.State().WasFocusLost());
}

TEST_CASE("the last device is stamped by whoever spoke and is an edge", "[input]") {
	// **Two frames and a compare, not one frame and a value.**
	// `LastInputTypeChanged` is the edge a place watches to swap "press E" for
	// "click here", and a translator that stamped the source but never rolled the
	// previous one would report a change on every frame — which passes any test
	// that only looks at `LastSource`.
	Translator translator;

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_E, true)));
	CHECK(translator.State().LastSource == InputSource::Keyboard);
	CHECK_FALSE(translator.State().WasLastSourceChanged());

	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(MotionEvent(4.0f, 4.0f, 1.0f, 1.0f)));
	CHECK(translator.State().LastSource == InputSource::MouseMovement);
	CHECK(translator.State().WasLastSourceChanged());

	// **Still the mouse on the next frame, and no longer a change.** The half a
	// naive implementation gets wrong.
	translator.BeginFrame();
	CHECK(translator.State().LastSource == InputSource::MouseMovement);
	CHECK_FALSE(translator.State().WasLastSourceChanged());

	// **The button and not `MouseButton1` for all three**, because
	// `IsMouseButtonPressed` and `InputObject.UserInputType` both rely on the
	// button ordinals being the first three `InputSource` ones.
	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(ButtonEvent(SDL_BUTTON_RIGHT, true)));
	CHECK(translator.State().LastSource == InputSource::MouseButton2);
	CHECK(translator.State().WasLastSourceChanged());

	// A release is the keyboard speaking too, so a place does not flicker its
	// prompts every time somebody lets go of a key.
	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_E, false)));
	CHECK(translator.State().LastSource == InputSource::Keyboard);

	// **Losing focus is not a device speaking.** Everything reads as released on
	// that frame, and a place that swapped its prompts on an alt-tab would be
	// reporting a keyboard nobody touched.
	translator.BeginFrame();
	REQUIRE(translator.HandleEvent(WindowEvent(false)));
	CHECK(translator.State().LastSource == InputSource::Keyboard);
	CHECK_FALSE(translator.State().WasLastSourceChanged());
}

TEST_CASE("typed text arrives as UTF-8 and is a frame delta", "[input][translate]") {
	// **The half of the keyboard that is not a keycode.** A key event says which
	// key moved; this says what it spelled, and the two are different questions
	// — the layout, the modifiers and any composition the platform ran all sit
	// between them, so `Shift` plus `1` is two key bits there and `!` here.
	//
	// **The assertion that matters is byte-exact.** `SDL_TextInputEvent` carries
	// UTF-8, and anything that took a byte at a time or assumed one event was one
	// letter would cut a codepoint in half the first time somebody typed in their
	// own language — which is the failure this case exists to catch and which no
	// ASCII-only check would.
	Translator translator;
	translator.BeginFrame();

	CHECK(translator.TypedText().empty());

	// `H`, then `é` in two bytes, then an emoji in four. Three events, because
	// several arrive in one frame and the text is all of them.
	REQUIRE(translator.HandleEvent(TextEvent("H")));
	REQUIRE(translator.HandleEvent(TextEvent("\xC3\xA9")));
	REQUIRE(translator.HandleEvent(TextEvent("\xF0\x9F\x98\x80")));

	CHECK(translator.TypedText() == "H\xC3\xA9\xF0\x9F\x98\x80");
	CHECK(translator.TypedText().size() == 7);

	// **Typing is the keyboard speaking**, exactly as a press and a release are —
	// a place that swapped its prompts on a key edge and not on a character
	// would report the wrong device for anybody using an input method.
	CHECK(translator.State().LastSource == InputSource::Keyboard);

	// **Cleared and not rolled**, which is the difference between a delta and a
	// level: a character was produced once and has no previous value to be an
	// edge against.
	translator.BeginFrame();
	CHECK(translator.TypedText().empty());

	// A key event in the same frame does not invent text, and text does not
	// invent a key: the two are independent halves of one keystroke.
	REQUIRE(translator.HandleEvent(KeyEvent(SDLK_A, true)));
	CHECK(translator.TypedText().empty());
	CHECK(translator.State().IsKeyDown(KeyCode::A));

	REQUIRE(translator.HandleEvent(TextEvent("a")));
	CHECK(translator.TypedText() == "a");

	// **Losing the window drops what was typed into it**, for the reason every
	// other delta is dropped there: the frame did not finish delivering.
	REQUIRE(translator.HandleEvent(WindowEvent(false)));
	CHECK(translator.TypedText().empty());
}
