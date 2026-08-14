// Typing into the box that has the keyboard.
//
// **Every case here is about a character rather than a byte**, because that is
// the one thing this file can get wrong silently: a caret counted in bytes puts
// text in the right place for as long as everybody types in English and cuts a
// letter in half the first time somebody does not. The fixtures are therefore
// deliberately accented and emoji-bearing, and the assertions are on both the
// string and `Entry::CursorPosition`.
//
// The second thing is the caret surviving a world it does not control: a script
// may replace `TextBox.Text` at any moment through a plain property write with
// no setter to hook, so every case that follows a shorter string is a case about
// not walking off the end.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.gui.typing")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	// `é` in two bytes and `😀` in four, spelled out so a case can say how many
	// of each it expects without a reader counting escapes.
	constexpr std::string_view ACCENT = "\xC3\xA9";
	constexpr std::string_view EMOJI = "\xF0\x9F\x98\x80";

	// A world with a `GuiService` and one `TextBox` in it.
	struct World {
		Store Data;
		Entity Box = NULL_ENTITY;

		explicit World(std::string_view name, std::string text = "") : Data(name) {
			RegisterGuiClasses();
			InstallGuiServices(Data);

			Box = Data.CreateInstance(GuiClass("TextBox"), "Entry");

			Label label;
			label.Text = std::move(text);
			Data.Set(Box, label);

			// Off, because most of these cases are about text that is already
			// there and a box that empties itself on focus has none.
			Entry entry;
			entry.ClearTextOnFocus = false;
			Data.Set(Box, entry);

			REQUIRE(Focus(Data, Box));
		}

		const std::string &Text() const {
			return Data.Get<Label>(Box)->Text;
		}

		int32_t Cursor() const {
			return Data.Get<Entry>(Box)->CursorPosition;
		}

		int32_t Anchor() const {
			return Data.Get<Entry>(Box)->SelectionStart;
		}

		// Puts the caret where a case wants it, in characters.
		void PlaceCaret(int32_t position, int32_t anchor = -1) {
			Entry *entry = Data.GetMutable<Entry>(Box);
			entry->CursorPosition = position;
			entry->SelectionStart = anchor;
		}

		TypeResult Send(const Typing &typing) {
			return Type(Data, typing);
		}

		TypeResult Send(std::string_view text) {
			Typing typing;
			typing.Text = text;
			return Type(Data, typing);
		}
	};
}

TEST_CASE("typing appends at the caret", "[gui][typing]") {
	World world("gui_typing.append", "hi");
	REQUIRE(world.Cursor() == 3);

	const TypeResult typed = world.Send("!");
	CHECK(typed.Instance == world.Box);
	CHECK(typed.Changed);
	CHECK_FALSE(typed.Released);
	CHECK(world.Text() == "hi!");
	CHECK(world.Cursor() == 4);

	// **Where the caret is and not where the string ends.** A box whose caret a
	// person moved back inserts in the middle, which is the whole difference
	// between a text field and an append-only log.
	world.PlaceCaret(1);
	world.Send("oh ");
	CHECK(world.Text() == "oh hi!");
	CHECK(world.Cursor() == 4);
}

TEST_CASE("nothing is typed into a world where nothing has focus", "[gui][typing]") {
	// **The ordinary frame**, which is every frame in a game with no text field
	// on screen: the host calls this unconditionally and it has to be a lookup
	// and a return rather than anything that touches a component.
	World world("gui_typing.unfocused", "hi");
	REQUIRE(Focus(world.Data, NULL_ENTITY));

	const TypeResult typed = world.Send("x");
	CHECK(typed.Instance == NULL_ENTITY);
	CHECK_FALSE(typed.Changed);
	CHECK(world.Text() == "hi");
}

TEST_CASE("a multi-byte character arrives whole", "[gui][typing]") {
	// **The failure this pins is one byte of a letter.** `Translator::TypedText`
	// hands over composed UTF-8 — two bytes for `é` and four for an emoji — and
	// anything on this path that measured the string in bytes would place the
	// next character inside the previous one.
	World world("gui_typing.utf8");

	world.Send("H");
	world.Send(ACCENT);
	world.Send(EMOJI);

	CHECK(world.Text() == std::string("H") + std::string(ACCENT) + std::string(EMOJI));
	CHECK(world.Text().size() == 7);

	// Three characters in seven bytes, so the caret is 4 and not 8.
	CHECK(world.Cursor() == 4);

	// And an insertion between two of them lands on the boundary rather than
	// inside the emoji.
	world.PlaceCaret(3);
	world.Send("-");
	CHECK(world.Text() == std::string("H") + std::string(ACCENT) + "-" + std::string(EMOJI));
}

TEST_CASE("backspace removes one character and not one byte", "[gui][typing]") {
	// Five characters in nine bytes: `h`, `é`, `l`, `l`, `😀`.
	World world("gui_typing.backspace", "h" + std::string(ACCENT) + "ll" + std::string(EMOJI));
	REQUIRE(world.Text().size() == 9);
	REQUIRE(world.Cursor() == 6);

	Typing typing;
	typing.Backspace = true;

	// The emoji goes as one thing. A byte-wise delete would leave three bytes of
	// it behind, which is not text in any encoding.
	CHECK(world.Send(typing).Changed);
	CHECK(world.Text() == "h" + std::string(ACCENT) + "ll");
	CHECK(world.Text().size() == 5);
	CHECK(world.Cursor() == 5);

	world.Send(typing);
	world.Send(typing);
	CHECK(world.Text() == "h" + std::string(ACCENT));
	CHECK(world.Text().size() == 3);
	CHECK(world.Cursor() == 3);

	// And the accented letter, which is the two-byte case.
	world.Send(typing);
	CHECK(world.Text() == "h");
	CHECK(world.Cursor() == 2);

	// At the start of the text it does nothing rather than under-running.
	world.Send(typing);
	CHECK(world.Text().empty());
	CHECK(world.Cursor() == 1);

	const TypeResult typed = world.Send(typing);
	CHECK_FALSE(typed.Changed);
	CHECK(world.Text().empty());
	CHECK(world.Cursor() == 1);
}

TEST_CASE("a script setting shorter text does not walk the caret off the end", "[gui][typing]") {
	// **The crash nobody writes a test for.** `TextBox.Text` is a plain property
	// write with no setter to hook, so a handler that replaces the text while a
	// person is typing leaves the caret pointing past the end of a string that is
	// now shorter — and every insertion point derived from it is out of range.
	// `Type` clamps at the reader, which is the only place that indexes by it.
	World world("gui_typing.shrunk", "a long piece of text");
	REQUIRE(world.Cursor() == 21);

	// What `box.Focused:Connect(function() box.Text = ... end)` does, which is
	// exactly the shape `examples/Interface.luau` already had.
	world.Data.GetMutable<Label>(world.Box)->Text = "hi";

	world.Send("!");
	CHECK(world.Text() == "hi!");
	CHECK(world.Cursor() == 4);

	// The same from the other side: a selection left pointing past the end.
	// Both ends clamp onto the last position, which collapses the selection —
	// so the backspace deletes one character rather than a range of nothing.
	world.Data.GetMutable<Label>(world.Box)->Text = "ab";
	world.PlaceCaret(40, 30);

	Typing typing;
	typing.Backspace = true;
	world.Send(typing);
	CHECK(world.Text() == "a");
	CHECK(world.Cursor() == 2);

	// And an empty box with a caret from a longer life.
	world.Data.GetMutable<Label>(world.Box)->Text.clear();
	world.PlaceCaret(9);
	world.Send("z");
	CHECK(world.Text() == "z");
	CHECK(world.Cursor() == 2);
}

TEST_CASE("a selection is replaced by what is typed and removed by backspace", "[gui][typing]") {
	World world("gui_typing.selection", "hello");

	// Characters 2 to 4 — `el` — with the anchor before the moving end.
	world.PlaceCaret(4, 2);
	world.Send("i");
	CHECK(world.Text() == "hilo");
	CHECK(world.Cursor() == 3);
	CHECK(world.Anchor() == -1);

	// **The other direction, which is what a drag leftwards produces.** A
	// selection is an anchor and a moving end rather than a start and a length,
	// so `SelectionStart` after `CursorPosition` is an ordinary state and not a
	// broken one.
	world.PlaceCaret(2, 4);
	Typing typing;
	typing.Backspace = true;
	CHECK(world.Send(typing).Changed);
	CHECK(world.Text() == "ho");
	CHECK(world.Cursor() == 2);
	CHECK(world.Anchor() == -1);
}

TEST_CASE("the caret moves in characters and shift extends a selection", "[gui][typing]") {
	World world("gui_typing.caret", "h" + std::string(ACCENT) + std::string(EMOJI));
	REQUIRE(world.Cursor() == 4);

	Typing left;
	left.Caret = -1;

	CHECK_FALSE(world.Send(left).Changed);
	CHECK(world.Cursor() == 3);
	CHECK(world.Anchor() == -1);

	// **Clamped at both ends**, so a held arrow at the edge is a no-op rather
	// than a caret at zero that the next insertion indexes with.
	world.Send(left);
	world.Send(left);
	world.Send(left);
	CHECK(world.Cursor() == 1);

	Typing extend;
	extend.Caret = 1;
	extend.Extend = true;

	world.Send(extend);
	CHECK(world.Anchor() == 1);
	CHECK(world.Cursor() == 2);

	world.Send(extend);
	CHECK(world.Anchor() == 1);
	CHECK(world.Cursor() == 3);

	// Typing over the extended selection removes both characters, which is the
	// proof the two ends are the ones the extension built.
	world.Send("x");
	CHECK(world.Text() == "x" + std::string(EMOJI));

	// An unshifted move onto a selection collapses onto the end it went towards
	// rather than stepping from the caret.
	world.PlaceCaret(3, 1);
	world.Send(left);
	CHECK(world.Cursor() == 1);
	CHECK(world.Anchor() == -1);
}

TEST_CASE("Return releases a single-line box and breaks a line in a multi-line one", "[gui][typing]") {
	World world("gui_typing.submit", "name");

	Typing submit;
	submit.Submit = true;

	const TypeResult typed = world.Send(submit);
	CHECK(typed.Released);
	CHECK(typed.Instance == world.Box);
	CHECK_FALSE(typed.Changed);
	CHECK(world.Text() == "name");

	// **Releasing is what puts the caret back to -1**, so the box reads as
	// unfocused everywhere rather than in one field and not the other.
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);
	CHECK(world.Cursor() == -1);
	CHECK(world.Anchor() == -1);

	// And a multi-line box takes the break instead, which is the only rule that
	// leaves both kinds of box usable.
	world.Data.GetMutable<Entry>(world.Box)->MultiLine = true;
	REQUIRE(Focus(world.Data, world.Box));

	const TypeResult broke = world.Send(submit);
	CHECK_FALSE(broke.Released);
	CHECK(broke.Changed);
	CHECK(world.Text() == "name\n");
	CHECK(world.Cursor() == 6);
	CHECK(FocusedTextBox(world.Data) == world.Box);
}

TEST_CASE("a locked box takes no text and still moves and releases", "[gui][typing]") {
	// `TextEditable` is what a game sets on a box it fills in itself. Roblox's
	// meaning is that the *text* is not a person's to change, not that the box
	// stops being one.
	World world("gui_typing.locked", "fixed");
	world.Data.GetMutable<Entry>(world.Box)->TextEditable = false;

	CHECK_FALSE(world.Send("x").Changed);
	CHECK(world.Text() == "fixed");

	Typing typing;
	typing.Backspace = true;
	CHECK_FALSE(world.Send(typing).Changed);
	CHECK(world.Text() == "fixed");

	Typing left;
	left.Caret = -1;
	world.Send(left);
	CHECK(world.Cursor() == 5);

	Typing submit;
	submit.Submit = true;
	CHECK(world.Send(submit).Released);
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);
}

TEST_CASE("one frame's keystrokes are applied in the order a person meant", "[gui][typing]") {
	// A frame carries a state rather than a stream — `Typing`'s own note — so
	// the order is this module's to state: characters, then Backspace, then the
	// caret, then Return. A frame that typed a letter and pressed Backspace ends
	// where it started rather than one character short of it.
	World world("gui_typing.order", "ab");

	Typing typing;
	typing.Text = "c";
	typing.Backspace = true;

	CHECK(world.Send(typing).Changed);
	CHECK(world.Text() == "ab");
	CHECK(world.Cursor() == 3);

	// And Return last, so a frame that typed and submitted carries what it typed.
	Typing finish;
	finish.Text = "!";
	finish.Submit = true;

	const TypeResult typed = world.Send(finish);
	CHECK(typed.Changed);
	CHECK(typed.Released);
	CHECK(world.Text() == "ab!");
}

TEST_CASE("a destroyed box takes no typing", "[gui][typing]") {
	// The dangling case from the typing side. `FocusedTextBox` validates the
	// handle on the way out, so this is a lookup that answers null rather than a
	// write through a recycled row.
	World world("gui_typing.destroyed", "hi");
	const Entity box = world.Box;

	world.Data.DestroyInstance(box);

	const TypeResult typed = Type(world.Data, Typing{"x", false, 0, false, false});
	CHECK(typed.Instance == NULL_ENTITY);
	CHECK_FALSE(typed.Changed);
}
