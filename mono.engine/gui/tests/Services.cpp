// `GuiService`, and the one behaviour it exists to give.
//
// **The selection is the whole reason this class is registered.**
// `GuiObject::Selectable` has been a declared, saved, bound property since the
// tree went in and nothing read it — which is the state the version's own rule
// refuses to leave a property in. These cases are that rule being satisfied:
// they fail if selection stops working, rather than merely if it stops
// compiling.
//
// `Path2D` and `GuidRegistryService` are deliberately absent and
// `gui/Services.hpp` gives the reason at length: there is no `DrawKind` a path
// could compile to and nothing for a GUID registry to keep, so both would be
// classes that do nothing forever with no error — the failure that kept
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

			Container = Data.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui"
			);

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
	// it got, exactly as it does `scene::InstallServices` — so a second call has
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

	// Clearing is always allowed — it is the one move that cannot strand
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
	// "above" selects a sibling when a player presses up — which reads as the
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
	// under a disabled collector is unreachable because it is not on screen —
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
