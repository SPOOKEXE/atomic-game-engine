// `GuiService`, and the one behaviour it exists to give.
//
// **The selection is the whole reason this class is registered.**
// `GuiObject::Selectable` has been a declared, saved, bound property since the
// tree went in and nothing read it - which is the state the version's own rule
// refuses to leave a property in. These cases are that rule being satisfied:
// they fail if selection stops working, rather than merely if it stops
// compiling.
//
// `Path2D` and `GuidRegistryService` are deliberately absent and
// `gui/Services.hpp` gives the reason at length: there is no `DrawKind` a path
// could compile to and nothing for a GUID registry to keep, so both would be
// classes that do nothing forever with no error - the failure that kept
// `VideoFrame` out.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.gui.services")

using Catch::Approx;
using engine::core::UDim2;
using engine::ecs::Entity;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	// A world with the tree, a `StarterGui` to draw from, and a `GuiService`.
	struct World {
		Store Data;
		CompileRequest Request;
		Compiled List;
		Entity Container;
		Entity Screen;

		explicit World(std::string_view name) : Data(name) {
			RegisterGuiClasses();
			InstallGuiServices(Data);

			Container =
				Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui");

			Request.Display.Width = 800.0f;
			Request.Display.Height = 600.0f;

			Screen = Data.CreateInstance(GuiClass("ScreenGui"), "ScreenGui");
			Data.SetParent(Screen, Container);
		}

		// A selectable button at a fixed rectangle, which is what makes the
		// direction cases readable: the geometry is the assertion.
		Entity Button(const char *name, float x, float y, float width, float height, bool selectable = true) {
			const Entity made = Data.CreateInstance(GuiClass("TextButton"), name);
			Data.SetParent(made, Screen);

			Element element;
			element.Position = UDim2{0.0f, x, 0.0f, y};
			element.Size = UDim2{0.0f, width, 0.0f, height};
			element.Selectable = selectable;
			Data.Set(made, element);
			return made;
		}

		const DrawList &Compile() {
			List.Rebuild(Data, Request);
			return List.Commands();
		}

		Entity Selected() {
			const Entity service = GuiServiceOf(Data);
			REQUIRE(service != engine::ecs::NULL_ENTITY);
			const GuiServiceState *state = Data.Get<GuiServiceState>(service);
			REQUIRE(state != nullptr);
			return state->SelectedObject;
		}
	};
}

TEST_CASE("installing the services is idempotent", "[gui][services]") {
	// The studio runs this after every load without checking which kind of file
	// it got, exactly as it does `scene::InstallServices` - so a second call has
	// to be a lookup rather than a second service.
	World world("gui_services.idempotent");

	const Entity first = GuiServiceOf(world.Data);
	REQUIRE(first != engine::ecs::NULL_ENTITY);

	CHECK(InstallGuiServices(world.Data) == first);
	CHECK(GuiServiceOf(world.Data) == first);
}

TEST_CASE("the gui inset is the screen's reserved strip", "[gui][services]") {
	// **Read from the screen rather than stored**, because the inset belongs to
	// the surface being drawn to. A service holding its own copy would be a
	// second answer, and it would drift the first time a panel resized.
	engine::gui::Screen screen;
	screen.TopInset = 36.0f;

	CHECK(GuiInset(screen).Y == Approx(36.0f));
	CHECK(GuiInset(screen).X == Approx(0.0f));

	// Zero by default, because this engine has no top bar of its own and a
	// reserved strip nothing occupies is dead space an author cannot explain.
	CHECK(GuiInset(engine::gui::Screen{}).Y == Approx(0.0f));
}

TEST_CASE("selection refuses an element that cannot hold it", "[gui][services]") {
	// **The state this refusal exists to prevent is unrecoverable by a
	// player.** A selection parked on something no direction can leave leaves a
	// gamepad with nothing to do but restart the game.
	World world("gui_services.refuse");

	const Entity plain = world.Button("Plain", 0.0f, 0.0f, 100.0f, 40.0f, false);
	CHECK_FALSE(Select(world.Data, plain));
	CHECK(world.Selected() == engine::ecs::NULL_ENTITY);

	const Entity ok = world.Button("Ok", 0.0f, 60.0f, 100.0f, 40.0f, true);
	CHECK(Select(world.Data, ok));
	CHECK(world.Selected() == ok);

	// Clearing is always allowed - it is the one move that cannot strand
	// anything.
	CHECK(Select(world.Data, engine::ecs::NULL_ENTITY));
	CHECK(world.Selected() == engine::ecs::NULL_ENTITY);
}

TEST_CASE("selection seeds from nothing in paint order", "[gui][services]") {
	World world("gui_services.seed");

	world.Button("First", 0.0f, 0.0f, 100.0f, 40.0f);
	world.Button("Second", 0.0f, 60.0f, 100.0f, 40.0f);

	const DrawList &list = world.Compile();
	CHECK(SelectNext(world.Data, list, SelectionMove::Down));
	CHECK(world.Data.InstanceNameOf(world.Selected()) == engine::core::Name("First"));
}

TEST_CASE("a game can refuse to have its selection seeded", "[gui][services]") {
	// `AutoSelectGuiEnabled` false is a game saying it drives selection itself.
	// Moving from nothing then does nothing rather than picking something the
	// game did not choose.
	World world("gui_services.noauto");
	world.Button("First", 0.0f, 0.0f, 100.0f, 40.0f);

	const Entity service = GuiServiceOf(world.Data);
	world.Data.GetMutable<GuiServiceState>(service)->AutoSelectGuiEnabled = false;

	const DrawList &list = world.Compile();
	CHECK_FALSE(SelectNext(world.Data, list, SelectionMove::Down));
	CHECK(world.Selected() == engine::ecs::NULL_ENTITY);
}

TEST_CASE("selection moves to the nearest element in the direction pressed", "[gui][services]") {
	// **A cross, so every direction has exactly one right answer and three
	// wrong ones.** A row or a column would let a move that ignored the axis
	// entirely still look correct.
	//
	//              North (100, 0)
	//   West (0,100)  Middle (100,100)  East (200,100)
	//              South (100, 200)
	World world("gui_services.cross");

	const Entity north = world.Button("North", 100.0f, 0.0f, 60.0f, 40.0f);
	const Entity west = world.Button("West", 0.0f, 100.0f, 60.0f, 40.0f);
	const Entity middle = world.Button("Middle", 100.0f, 100.0f, 60.0f, 40.0f);
	const Entity east = world.Button("East", 200.0f, 100.0f, 60.0f, 40.0f);
	const Entity south = world.Button("South", 100.0f, 200.0f, 60.0f, 40.0f);

	const DrawList &list = world.Compile();

	const auto from = [&](Entity start, SelectionMove move) {
		REQUIRE(Select(world.Data, start));
		REQUIRE(SelectNext(world.Data, list, move));
		return world.Selected();
	};

	CHECK(from(middle, SelectionMove::Up) == north);
	CHECK(from(middle, SelectionMove::Down) == south);
	CHECK(from(middle, SelectionMove::Left) == west);
	CHECK(from(middle, SelectionMove::Right) == east);
}

TEST_CASE("selection does not move past the edge", "[gui][services]") {
	// Nothing in the direction pressed means the selection stays where it is,
	// rather than wrapping or clearing. Both alternatives are worse: wrapping
	// jumps the cursor across the screen and clearing strands the player.
	World world("gui_services.edge");

	const Entity only = world.Button("Only", 100.0f, 100.0f, 60.0f, 40.0f);
	const DrawList &list = world.Compile();

	REQUIRE(Select(world.Data, only));
	CHECK_FALSE(SelectNext(world.Data, list, SelectionMove::Up));
	CHECK(world.Selected() == only);
}

TEST_CASE("a level neighbour is not above anything", "[gui][services]") {
	// **Strictly forward, and this is the case that pins it.** Two buttons in a
	// row have equal Y, so an implementation testing "not below" rather than
	// "above" selects a sibling when a player presses up - which reads as the
	// stick being drifting rather than as a bug.
	World world("gui_services.level");

	const Entity left = world.Button("Left", 0.0f, 100.0f, 60.0f, 40.0f);
	world.Button("Right", 100.0f, 100.0f, 60.0f, 40.0f);

	const DrawList &list = world.Compile();
	REQUIRE(Select(world.Data, left));
	CHECK_FALSE(SelectNext(world.Data, list, SelectionMove::Up));
	CHECK(world.Selected() == left);
}

TEST_CASE("alignment breaks a tie without overruling distance", "[gui][services]") {
	// **The weighting is what makes this a tiebreak rather than a second
	// axis.** `Near` is slightly off to the side and close; `Far` is perfectly
	// aligned and much further. A player pressing up means the near one, and an
	// implementation that scored alignment equally with distance would jump
	// across the screen to the aligned one.
	World world("gui_services.tiebreak");

	const Entity start = world.Button("Start", 100.0f, 300.0f, 60.0f, 40.0f);
	const Entity near = world.Button("Near", 130.0f, 240.0f, 60.0f, 40.0f);
	world.Button("Far", 100.0f, 0.0f, 60.0f, 40.0f);

	const DrawList &list = world.Compile();
	REQUIRE(Select(world.Data, start));
	REQUIRE(SelectNext(world.Data, list, SelectionMove::Up));
	CHECK(world.Selected() == near);
}

TEST_CASE("an element that is not drawn cannot be selected into", "[gui][services]") {
	// **Candidates come from the compiled list, not the tree**, so an element
	// under a disabled collector is unreachable because it is not on screen -
	// which is the same reason `Pick` walks the list rather than descending.
	World world("gui_services.hidden");

	const Entity visible = world.Button("Visible", 100.0f, 200.0f, 60.0f, 40.0f);

	// A second screen, disabled, holding something that would otherwise be the
	// obvious answer to "up".
	const Entity other = world.Data.CreateInstance(GuiClass("ScreenGui"), "Hidden");
	world.Data.SetParent(other, world.Container);
	world.Data.Set(other, Layer{});
	world.Data.GetMutable<Layer>(other)->Enabled = false;

	const Entity buried = world.Data.CreateInstance(GuiClass("TextButton"), "Buried");
	world.Data.SetParent(buried, other);
	Element element;
	element.Position = UDim2{0.0f, 100.0f, 0.0f, 0.0f};
	element.Size = UDim2{0.0f, 60.0f, 0.0f, 40.0f};
	element.Selectable = true;
	world.Data.Set(buried, element);

	const DrawList &list = world.Compile();
	REQUIRE(Select(world.Data, visible));
	CHECK_FALSE(SelectNext(world.Data, list, SelectionMove::Up));
	CHECK(world.Selected() == visible);
}

namespace {
	// A world with a `StarterGui` root and one player holding a `PlayerGui`.
	//
	// **Both built by hand rather than by `scene::InstallServices`**, because
	// `gui` may not link `scene` - the refusal `STARTER_GUI` exists because of.
	// What the names have to be is exactly what that constant says, and
	// `examples/tests/Scene.cpp` is where the two spellings are pinned together.
	struct SpawnWorld {
		Store Data;
		Entity Starter;
		Entity Player;
		Entity PlayerGui;

		explicit SpawnWorld(std::string_view name) : Data(name) {
			RegisterGuiClasses();

			const auto plain = engine::ecs::Classes::Find(engine::core::Name("Instance"));
			Starter = Data.CreateInstance(plain, std::string(STARTER_GUI));

			Player = Data.CreateInstance(plain, "Somebody");
			PlayerGui = Data.CreateInstance(plain, std::string(PLAYER_GUI));
			Data.SetParent(PlayerGui, Player);
		}

		Entity Collector(Entity parent, const char *name, bool resetOnSpawn) {
			const Entity made = Data.CreateInstance(GuiClass("ScreenGui"), name);
			Data.SetParent(made, parent);

			Layer layer;
			layer.ResetOnSpawn = resetOnSpawn;
			Data.Set(made, layer);
			return made;
		}

		size_t CountIn(Entity parent) const {
			size_t found = 0;
			Data.EachChild(parent, [&](Entity) { found++; });
			return found;
		}

		Entity FindIn(Entity parent, const char *name) const {
			return Data.FindFirstChild(parent, name);
		}
	};
}

TEST_CASE("a spawn copies the template into the player's own container", "[gui][services]") {
	// **`StarterGui` is a template and what a player sees is their copy**, which
	// this engine did not have at all: `Layout` drew a `ScreenGui` from
	// `StarterGui` *or* from a `PlayerGui`, so every client drew the same
	// instances. That works in single player and is wrong the moment there are
	// two - a script hiding one player's health bar hid everybody's.
	SpawnWorld world("gui.spawn.copy");
	world.Collector(world.Starter, "Hud", true);
	world.Collector(world.Starter, "Menu", true);

	REQUIRE(world.CountIn(world.PlayerGui) == 0);
	CHECK(ResetPlayerGui(world.Data, world.Player) == 2);
	CHECK(world.CountIn(world.PlayerGui) == 2);
	CHECK(world.FindIn(world.PlayerGui, "Hud") != engine::ecs::NULL_ENTITY);

	// **The template is still there**, which is what "cloned rather than moved"
	// buys: the next player and the next life need it.
	CHECK(world.CountIn(world.Starter) == 2);
}

TEST_CASE("a respawn replaces what resets and leaves what does not", "[gui][services]") {
	// **The whole of what `ResetOnSpawn` is for, and the field had been on
	// `Layer` since v0.8 with nothing reading it.** A minimap or a settings panel
	// carrying script state must survive a death; a health bar must not.
	SpawnWorld world("gui.spawn.reset");
	world.Collector(world.Starter, "Hud", true);
	world.Collector(world.Starter, "Minimap", false);

	REQUIRE(ResetPlayerGui(world.Data, world.Player) == 2);

	const Entity firstHud = world.FindIn(world.PlayerGui, "Hud");
	const Entity firstMap = world.FindIn(world.PlayerGui, "Minimap");
	REQUIRE(firstHud != engine::ecs::NULL_ENTITY);
	REQUIRE(firstMap != engine::ecs::NULL_ENTITY);

	// **Only the resetting one is copied again**, because the other survived and
	// a survivor of the same name is the copy the player already has. Cloning
	// beside it would leave them holding two, one of which nothing updates.
	CHECK(ResetPlayerGui(world.Data, world.Player) == 1);
	CHECK(world.CountIn(world.PlayerGui) == 2);

	// **The identities are the assertion, not the count.** The health bar is a
	// *different* instance - destroyed and re-cloned, so a script's leftover
	// state goes with it - and the minimap is the same one, still holding
	// whatever a script put in it.
	CHECK(world.FindIn(world.PlayerGui, "Hud") != firstHud);
	CHECK(world.FindIn(world.PlayerGui, "Minimap") == firstMap);
}

TEST_CASE("a spawn with nothing to copy is not a failure", "[gui][services]") {
	// Three honest zeroes: a world with no template, a player with no container,
	// and a template that is empty. None of them is a mistake a host should have
	// to guard against before calling.
	SpawnWorld world("gui.spawn.empty");
	CHECK(ResetPlayerGui(world.Data, world.Player) == 0);

	const Entity loose =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "NoGui");
	CHECK(ResetPlayerGui(world.Data, loose) == 0);
}

TEST_CASE("what is not a collector is copied and never cleared", "[gui][services]") {
	// **`ResetOnSpawn` describes a *collector's* lifetime**, so anything without
	// a `Layer` is left where it is. Roblox copies a container's contents rather
	// than a filtered set, and a rule that filtered would silently drop the one
	// thing a game had put there.
	SpawnWorld world("gui.spawn.plain");

	const auto plain = engine::ecs::Classes::Find(engine::core::Name("Instance"));
	const Entity assets = world.Data.CreateInstance(plain, "Assets");
	world.Data.SetParent(assets, world.Starter);

	REQUIRE(ResetPlayerGui(world.Data, world.Player) == 1);
	const Entity copied = world.FindIn(world.PlayerGui, "Assets");
	REQUIRE(copied != engine::ecs::NULL_ENTITY);

	// Not cleared on the next spawn, and therefore not copied again either -
	// it survives by the same rule a `ResetOnSpawn = false` collector does.
	CHECK(ResetPlayerGui(world.Data, world.Player) == 0);
	CHECK(world.FindIn(world.PlayerGui, "Assets") == copied);
}

TEST_CASE("two players hold two interfaces and three spawns keep them apart", "[gui][services]") {
	// **The multiplayer half, which is the whole reason `StarterGui` is a
	// template.** A one-player check passes against the version this replaced,
	// where every client drew the template's own instances - so a script hiding
	// one player's health bar hid everybody's.
	//
	// **Three spawns rather than two**, because a survivor has to keep its
	// identity for as long as it lives and not merely across the first death: a
	// reset that re-cloned every other time would pass a two-spawn test.
	SpawnWorld world("gui.spawn.two");

	const auto plain = engine::ecs::Classes::Find(engine::core::Name("Instance"));
	const Entity second = world.Data.CreateInstance(plain, "Somebody Else");
	const Entity secondGui = world.Data.CreateInstance(plain, std::string(PLAYER_GUI));
	world.Data.SetParent(secondGui, second);

	world.Collector(world.Starter, "Hud", true);
	world.Collector(world.Starter, "Minimap", false);

	REQUIRE(ResetPlayerGui(world.Data, world.Player) == 2);
	REQUIRE(ResetPlayerGui(world.Data, second) == 2);

	// Four copies of two templates, and no two of them are the same instance.
	const Entity mineHud = world.FindIn(world.PlayerGui, "Hud");
	const Entity mineMap = world.FindIn(world.PlayerGui, "Minimap");
	const Entity theirsHud = world.FindIn(secondGui, "Hud");
	const Entity theirsMap = world.FindIn(secondGui, "Minimap");

	REQUIRE(mineHud != engine::ecs::NULL_ENTITY);
	REQUIRE(theirsHud != engine::ecs::NULL_ENTITY);
	CHECK(mineHud != theirsHud);
	CHECK(mineMap != theirsMap);
	CHECK(mineHud != world.FindIn(world.Starter, "Hud"));

	// Two more lives for the first player, and the second's is untouched by
	// either - a reset that reached the template rather than the copy would
	// show up here and nowhere else.
	CHECK(ResetPlayerGui(world.Data, world.Player) == 1);
	CHECK(ResetPlayerGui(world.Data, world.Player) == 1);

	CHECK(world.CountIn(world.PlayerGui) == 2);
	CHECK(world.CountIn(secondGui) == 2);
	CHECK(world.FindIn(world.PlayerGui, "Minimap") == mineMap);
	CHECK(world.FindIn(world.PlayerGui, "Hud") != mineHud);
	CHECK(world.FindIn(secondGui, "Hud") == theirsHud);
	CHECK(world.FindIn(secondGui, "Minimap") == theirsMap);

	// The template is still exactly two, after five resets.
	CHECK(world.CountIn(world.Starter) == 2);
}

// --- the keyboard focus -----------------------------------------------------
//
// **Where the focus lives is the decision under test, not where it is set
// from.** `gui::Router` decides it and these cases do not go through the router
// at all: what they pin is that `GuiServiceState::FocusedTextBox` is the one
// place the answer rests, that a handle is validated rather than trusted, and
// that the caret is counted in characters.

namespace {
	// A `TextBox` with text in it, which is the fixture every case below wants.
	//
	// `ClearTextOnFocus` is off by default here and on by default in the
	// component, because most of these cases are about the caret and a box that
	// empties itself has nothing to count.
	Entity TextBox(World &world, const char *name, std::string text, bool clearOnFocus = false) {
		const Entity made = world.Data.CreateInstance(GuiClass("TextBox"), name);
		world.Data.SetParent(made, world.Screen);

		Label label;
		label.Text = std::move(text);
		world.Data.Set(made, label);

		Entry entry;
		entry.ClearTextOnFocus = clearOnFocus;
		world.Data.Set(made, entry);
		return made;
	}
}

TEST_CASE("the keyboard goes to a text box and to nothing else", "[gui][services]") {
	World world("gui_services.focus");
	const Entity box = TextBox(world, "Entry", "hi");
	const Entity button = world.Button("Button", 0.0f, 0.0f, 10.0f, 10.0f);

	CHECK(FocusedTextBox(world.Data) == engine::ecs::NULL_ENTITY);

	// **A `TextButton` is refused**, for the reason an unselectable element is
	// refused by `Select`: focus parked on something that cannot take a
	// character is a state nothing can leave.
	CHECK_FALSE(Focus(world.Data, button));
	CHECK(FocusedTextBox(world.Data) == engine::ecs::NULL_ENTITY);

	CHECK(Focus(world.Data, box));
	CHECK(FocusedTextBox(world.Data) == box);

	// Focusing what is already focused changes nothing, so a caller that calls
	// this every frame does not produce an event every frame.
	CHECK_FALSE(Focus(world.Data, box));

	CHECK(Focus(world.Data, engine::ecs::NULL_ENTITY));
	CHECK(FocusedTextBox(world.Data) == engine::ecs::NULL_ENTITY);
}

TEST_CASE("a destroyed text box is not focused and a reparented one still is", "[gui][services]") {
	// **The dangling case, which is the failure mode this design is arranged
	// around.** The handle carries a generation, so the answer is a question
	// rather than a use-after-free - and nothing had to hook `DestroyInstance`
	// for that to be true.
	World world("gui_services.focus_lifetime");
	const Entity box = TextBox(world, "Entry", "hi");
	const Entity elsewhere = world.Data.CreateInstance(GuiClass("ScreenGui"), "Other");
	world.Data.SetParent(elsewhere, world.Container);

	REQUIRE(Focus(world.Data, box));

	// **Moving does not interrupt typing.** A handle does not change when its
	// instance is reparented, and nothing the person did says they stopped.
	REQUIRE(world.Data.SetParent(box, elsewhere));
	CHECK(FocusedTextBox(world.Data) == box);

	world.Data.DestroyInstance(box);
	CHECK(FocusedTextBox(world.Data) == engine::ecs::NULL_ENTITY);

	// And the world is focusable again afterwards, which a stale handle left in
	// place would not be: `Focus` compares against what `FocusedTextBox`
	// answers, so a dead box would still equal itself and refuse the next one.
	const Entity replacement = TextBox(world, "Entry2", "");
	CHECK(Focus(world.Data, replacement));
	CHECK(FocusedTextBox(world.Data) == replacement);
}

TEST_CASE("taking focus places the caret in characters, not in bytes", "[gui][services]") {
	// **`Entry::CursorPosition` is one-based and counted in characters**, which
	// is Roblox's number. `Label::Text` is UTF-8, so a caret derived from
	// `Text.size()` sits past the end of anything typed in a language with
	// accents in it - one place too far for `é` and three for an emoji.
	World world("gui_services.caret");

	// Five characters in nine bytes: `h`, `é` (two), `l`, `l`, `😀` (four).
	const Entity box = TextBox(world, "Entry", "h\xC3\xA9ll\xF0\x9F\x98\x80");
	REQUIRE(world.Data.Get<Label>(box)->Text.size() == 9);

	REQUIRE(Focus(world.Data, box));
	CHECK(world.Data.Get<Entry>(box)->CursorPosition == 6);

	// Releasing puts it back to what "unfocused" means in that field.
	REQUIRE(Focus(world.Data, engine::ecs::NULL_ENTITY));
	CHECK(world.Data.Get<Entry>(box)->CursorPosition == -1);
	CHECK(world.Data.Get<Entry>(box)->SelectionStart == -1);
}

TEST_CASE("ClearTextOnFocus empties the box at the moment focus is taken", "[gui][services]") {
	// The property has been declared, saved and bound since the tree went in
	// and nothing read it - the state this version's rule refuses to leave a
	// property in, and the same argument `Selectable` is here for.
	World world("gui_services.clear_on_focus");
	const Entity clearing = TextBox(world, "Search", "last search", true);
	const Entity keeping = TextBox(world, "Name", "Ada", false);

	REQUIRE(Focus(world.Data, clearing));
	CHECK(world.Data.Get<Label>(clearing)->Text.empty());

	// **The caret is counted after the text is decided**, so an emptied box
	// reads 1 rather than the twelve its old contents would have given.
	CHECK(world.Data.Get<Entry>(clearing)->CursorPosition == 1);

	// Moving the focus releases the first box and leaves its text alone: what
	// `ClearTextOnFocus` describes is what happens on the way *in*.
	REQUIRE(Focus(world.Data, keeping));
	CHECK(world.Data.Get<Entry>(clearing)->CursorPosition == -1);
	CHECK(world.Data.Get<Label>(keeping)->Text == "Ada");
	CHECK(world.Data.Get<Entry>(keeping)->CursorPosition == 4);
}
