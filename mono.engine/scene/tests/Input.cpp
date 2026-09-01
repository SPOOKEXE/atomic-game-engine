#include <engine/scene/Input.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

TEST_SUITE_ID("engine.scene.input")

using engine::scene::Describe;
using engine::scene::InputSource;
using engine::scene::InputState;
using engine::scene::KeyBits;
using engine::scene::KeyCode;
using engine::scene::KeyFromName;
using engine::scene::MouseBehavior;
using engine::scene::MouseButton;

// **The invariant `scene/AGENTS.md` states and nothing checked.** A component is
// serialised as its object representation - `Column::Write` sends `sizeof(T)`
// bytes and does not know which of them a member claimed - so a byte the
// compiler inserted and nobody declared reaches a save file uninitialised. Every
// other component in the module names its padding; this one stopped six bytes
// short, which is what this case exists to keep from happening again.
//
// `static_assert` rather than a runtime `CHECK`, because the failure is a fact
// about the type and should stop the build that introduced it rather than a run
// that happens to execute this file.
static_assert(
	offsetof(InputState, Reserved) + sizeof(InputState::Reserved) == sizeof(InputState),
	"InputState::Reserved must reach the end of the object, or the bytes past it are unnamed "
	"padding that Column::Write puts in a save file uninitialised"
);

TEST_CASE("a key's bit survives a round trip", "[scene][input]") {
	KeyBits bits;
	CHECK_FALSE(bits.Any());

	bits.Set(KeyCode::W, true);
	CHECK(bits.Has(KeyCode::W));
	CHECK(bits.Any());

	// The neighbour, because a shift computed from the wrong ordinal sets a bit
	// one along and every single-key case still passes.
	CHECK_FALSE(bits.Has(KeyCode::V));
	CHECK_FALSE(bits.Has(KeyCode::X));

	bits.Set(KeyCode::W, false);
	CHECK_FALSE(bits.Has(KeyCode::W));
	CHECK_FALSE(bits.Any());
}

TEST_CASE("clearing one key leaves the others", "[scene][input]") {
	KeyBits bits;
	bits.Set(KeyCode::A, true);
	bits.Set(KeyCode::Space, true);

	bits.Set(KeyCode::A, false);
	CHECK_FALSE(bits.Has(KeyCode::A));
	CHECK(bits.Has(KeyCode::Space));
	CHECK(bits.Any());
}

TEST_CASE("a key past the end is refused rather than corrupting a word", "[scene][input]") {
	// `Count` is not a key. Setting it would index one word past the array, and
	// the guard is what keeps a bad ordinal from being a stray write into
	// whatever follows the bitset in the resource.
	KeyBits bits;
	bits.Set(KeyCode::Count, true);
	CHECK_FALSE(bits.Any());
	CHECK_FALSE(bits.Has(KeyCode::Count));
}

TEST_CASE("a key edge is the difference between the two frames", "[scene][input]") {
	InputState state;

	// Frame one: pressed. Down and not previously down.
	state.Down.Set(KeyCode::Space, true);
	CHECK(state.IsKeyDown(KeyCode::Space));
	CHECK(state.WasKeyPressed(KeyCode::Space));
	CHECK_FALSE(state.WasKeyReleased(KeyCode::Space));

	// Frame two: still held. **Held is not pressed** - a bound action fires on
	// the edge, and a held key reporting a press every frame is what turns one
	// jump into a flight.
	state.Previous = state.Down;
	CHECK(state.IsKeyDown(KeyCode::Space));
	CHECK_FALSE(state.WasKeyPressed(KeyCode::Space));
	CHECK_FALSE(state.WasKeyReleased(KeyCode::Space));

	// Frame three: released.
	state.Down.Set(KeyCode::Space, false);
	CHECK_FALSE(state.IsKeyDown(KeyCode::Space));
	CHECK_FALSE(state.WasKeyPressed(KeyCode::Space));
	CHECK(state.WasKeyReleased(KeyCode::Space));
}

TEST_CASE("a mouse edge is the difference between the two frames", "[scene][input]") {
	// The button half of the case above, and it is the half that had no reader
	// until the input pump grew one: `PumpInput` walks `KeyCode` and a mouse
	// button never produced an `InputBegan`.
	InputState state;
	const auto bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(MouseButton::Left));

	state.Buttons = bit;
	CHECK(state.IsButtonDown(MouseButton::Left));
	CHECK(state.WasButtonPressed(MouseButton::Left));
	CHECK_FALSE(state.WasButtonReleased(MouseButton::Left));

	state.PreviousButtons = state.Buttons;
	CHECK(state.IsButtonDown(MouseButton::Left));
	CHECK_FALSE(state.WasButtonPressed(MouseButton::Left));
	CHECK_FALSE(state.WasButtonReleased(MouseButton::Left));

	state.Buttons = 0;
	CHECK_FALSE(state.IsButtonDown(MouseButton::Left));
	CHECK_FALSE(state.WasButtonPressed(MouseButton::Left));
	CHECK(state.WasButtonReleased(MouseButton::Left));
}

TEST_CASE("one button's edge is not another's", "[scene][input]") {
	// A mask built from the wrong ordinal reports the right answer for the left
	// button and the wrong one for every other, which a single-button case
	// cannot tell apart.
	InputState state;
	state.Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(MouseButton::Right));

	CHECK(state.WasButtonPressed(MouseButton::Right));
	CHECK_FALSE(state.WasButtonPressed(MouseButton::Left));
	CHECK_FALSE(state.WasButtonPressed(MouseButton::Middle));
	CHECK_FALSE(state.IsButtonDown(MouseButton::Left));
}

TEST_CASE("a fresh state reports nothing down", "[scene][input]") {
	// The first frame: nothing has been measured, and every question about an
	// edge has to answer no rather than reading an unwritten `Previous`.
	const InputState state;
	CHECK_FALSE(state.Down.Any());
	CHECK_FALSE(state.Previous.Any());
	CHECK_FALSE(state.IsKeyDown(KeyCode::W));
	CHECK_FALSE(state.WasKeyPressed(KeyCode::W));
	CHECK_FALSE(state.WasKeyReleased(KeyCode::W));
	CHECK_FALSE(state.IsButtonDown(MouseButton::Left));
	CHECK_FALSE(state.WasButtonPressed(MouseButton::Left));
	CHECK_FALSE(state.WasButtonReleased(MouseButton::Left));
	CHECK(state.Focused);
	CHECK(state.Behaviour == MouseBehavior::Default);
	CHECK(state.WheelDelta == 0.0f);

	// **The two that travel towards the window default to what a window does
	// without being asked**, so a world nobody has scripted draws its pointer and
	// leaves it free.
	CHECK(state.MouseIconEnabled);
	CHECK(state.LastSource == InputSource::Keyboard);
	CHECK_FALSE(state.WasLastSourceChanged());
	CHECK_FALSE(state.HasFrameEvents());
}

TEST_CASE("frame input work is gated by whole-state edges", "[scene][input]") {
	InputState state;

	state.Down.Set(KeyCode::W, true);
	CHECK(state.HasFrameEvents());
	state.Previous = state.Down;
	CHECK_FALSE(state.HasFrameEvents());

	state.Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(MouseButton::Right));
	CHECK(state.HasFrameEvents());
	state.PreviousButtons = state.Buttons;
	CHECK_FALSE(state.HasFrameEvents());

	state.MouseDelta.X = 1.0f;
	CHECK(state.HasFrameEvents());
	state.MouseDelta = {};
	state.WheelDelta = -1.0f;
	CHECK(state.HasFrameEvents());
	state.WheelDelta = 0.0f;

	state.Focused = false;
	CHECK(state.HasFrameEvents());
	state.PreviousFocused = false;
	state.LastSource = InputSource::MouseMovement;
	CHECK(state.HasFrameEvents());
}

TEST_CASE("the device change is an edge and not a value", "[scene][input]") {
	// **`LastInputTypeChanged` is the difference between two frames**, exactly as
	// the focus pair is - a place swaps "press E" for "click here" on the moment
	// the answer changed, not on the answer. A state that reported a change while
	// the two agreed would fire it every frame.
	InputState state;
	state.LastSource = InputSource::MouseMovement;
	CHECK(state.WasLastSourceChanged());

	state.PreviousLastSource = state.LastSource;
	CHECK_FALSE(state.WasLastSourceChanged());

	// And it changes back, which is the case a one-way flag would miss.
	state.LastSource = InputSource::Keyboard;
	CHECK(state.WasLastSourceChanged());
}

TEST_CASE("every key name round-trips", "[scene][input]") {
	// `Describe` and `KeyFromName` are inverses, and the table is one list - a
	// key inserted in the middle of the enum without a name beside it shifts
	// every name after it by one, so `Enum.KeyCode.W` resolves to V.
	for (size_t ordinal = 0; ordinal < static_cast<size_t>(KeyCode::Count); ordinal++) {
		const auto key = static_cast<KeyCode>(ordinal);
		const char *name = Describe(key);
		REQUIRE(name != nullptr);
		CHECK(KeyFromName(name) == key);
	}
}

TEST_CASE("key names are distinct", "[scene][input]") {
	// Two keys sharing a name makes `KeyFromName` answer one of them, and which
	// one depends on the table's order rather than on anything a caller said.
	std::unordered_set<std::string> seen;
	for (size_t ordinal = 0; ordinal < static_cast<size_t>(KeyCode::Count); ordinal++) {
		const char *name = Describe(static_cast<KeyCode>(ordinal));
		CHECK(seen.insert(std::string(name)).second);
	}
}

TEST_CASE("controller buttons have key names without growing InputState", "[scene][input][gamepad]") {
	CHECK(sizeof(InputState) == 56);
	CHECK(engine::scene::KeyOf(engine::scene::ControllerButton::A) == KeyCode::ButtonA);
	CHECK(engine::scene::KeyOf(engine::scene::ControllerButton::RightTrigger) == KeyCode::ButtonR2);

	engine::scene::ControllerState controllers;
	auto &slot = controllers.Slots[0];
	slot.Connected = true;
	slot.Buttons = 1u << static_cast<uint8_t>(engine::scene::ControllerButton::A);
	controllers.LatchPresses();
	CHECK(slot.IsDown(engine::scene::ControllerButton::A));
	CHECK(controllers.AnyConnected());
	controllers.ConsumeTaps();
	CHECK(slot.PressedButtons == 0);
}

TEST_CASE("an unknown name is Unknown rather than a guess", "[scene][input]") {
	CHECK(KeyFromName("") == KeyCode::Unknown);
	CHECK(KeyFromName("NotAKey") == KeyCode::Unknown);

	// Case matters: the names are the surface a script compares against, so a
	// lookup that quietly accepted another spelling would make two spellings
	// work and only one of them documented.
	CHECK(KeyFromName("w") == KeyCode::Unknown);
	CHECK(KeyFromName("W") == KeyCode::W);
}

TEST_CASE("every mouse button has a name", "[scene][input]") {
	for (const MouseButton button : {MouseButton::Left, MouseButton::Right, MouseButton::Middle}) {
		const char *name = Describe(button);
		REQUIRE(name != nullptr);
		CHECK(std::string_view(name) != "?");
	}
}

TEST_CASE("an input source names every button and the three that are not", "[scene][input]") {
	// **The overlap is what `IsMouseButtonPressed` casts through.** It resolves
	// an `Enum.UserInputType` member to an ordinal and hands it to
	// `IsButtonDown`, so a source inserted ahead of the buttons would make "is
	// the left button down" answer about the right one - with nothing failing,
	// because the arithmetic still lands on a valid bit.
	CHECK(std::string_view(Describe(InputSource::MouseButton1)) == Describe(MouseButton::Left));
	CHECK(std::string_view(Describe(InputSource::MouseButton2)) == Describe(MouseButton::Right));
	CHECK(std::string_view(Describe(InputSource::MouseButton3)) == Describe(MouseButton::Middle));

	CHECK(std::string_view(Describe(InputSource::Keyboard)) == "Keyboard");
	CHECK(std::string_view(Describe(InputSource::MouseMovement)) == "MouseMovement");
	CHECK(std::string_view(Describe(InputSource::MouseWheel)) == "MouseWheel");

	// Distinct, for the reason the key names are: two sources sharing a name
	// makes an `InputObject` report one of them and which depends on the table.
	std::unordered_set<std::string> seen;
	for (size_t ordinal = 0; ordinal < static_cast<size_t>(InputSource::Count); ordinal++) {
		CHECK(seen.insert(std::string(Describe(static_cast<InputSource>(ordinal)))).second);
	}
}

TEST_CASE("focus is an edge and not a level", "[scene][input]") {
	// **`WindowFocused` and `WindowFocusReleased` are edges**, so the state has
	// to remember last frame's focus the way it remembers last frame's keys. A
	// fresh state has focus and had it, which reports neither edge - otherwise
	// the first pump of every world would fire a focus event nobody caused.
	InputState state;
	CHECK_FALSE(state.WasFocusGained());
	CHECK_FALSE(state.WasFocusLost());

	// Losing it. The frame this happens on is also the frame every held key
	// reports released, because the translator clears `Down` and leaves
	// `Previous` alone - so both are observable from one state.
	state.Previous = state.Down;
	state.Down.Set(KeyCode::W, true);
	state.PreviousFocused = state.Focused;
	state.Focused = false;
	state.Previous = state.Down;
	state.Down = KeyBits{};

	CHECK(state.WasFocusLost());
	CHECK_FALSE(state.WasFocusGained());
	CHECK(state.WasKeyReleased(KeyCode::W));

	// Regaining it, one frame later.
	state.PreviousFocused = state.Focused;
	state.Focused = true;
	CHECK(state.WasFocusGained());
	CHECK_FALSE(state.WasFocusLost());

	// And the frame after, which is a level rather than an edge.
	state.PreviousFocused = state.Focused;
	CHECK_FALSE(state.WasFocusGained());
	CHECK_FALSE(state.WasFocusLost());
}
