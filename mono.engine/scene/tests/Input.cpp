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
using engine::scene::InputState;
using engine::scene::KeyBits;
using engine::scene::KeyCode;
using engine::scene::KeyFromName;
using engine::scene::MouseBehavior;
using engine::scene::MouseButton;

// **The invariant `scene/AGENTS.md` states and nothing checked.** A component is
// serialised as its object representation — `Column::Write` sends `sizeof(T)`
// bytes and does not know which of them a member claimed — so a byte the
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

	// Frame two: still held. **Held is not pressed** — a bound action fires on
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
}

TEST_CASE("every key name round-trips", "[scene][input]") {
	// `Describe` and `KeyFromName` are inverses, and the table is one list — a
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
