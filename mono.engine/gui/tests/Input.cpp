#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.input")

using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	struct World {
		Store Data;
		CompileRequest Request;
		Compiled List;
		Router Route;

		explicit World(std::string_view name) : Data(name) {
			RegisterGuiClasses();

			// **The `GuiService`, because that is where the keyboard focus
			// lives.** A world without one routes the pointer exactly as it did
			// before focus existed and takes none at all — `gui::Focus`'s stated
			// answer — so a fixture that skipped this would pass every focus
			// case below by never focusing anything.
			InstallGuiServices(Data);

			// **A bare `Instance`, because `StarterGui` is not a `gui` class.**
			// It is `scene`'s service and this module may not link `scene`; what
			// the containment test reads is the *name*, so an instance carrying
			// that name is exactly as contained as the real service would be.
			// That is the same duplication `NormalId` already makes here.
			Container =
				Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui");
			Request.Display.Width = 800.0f;
			Request.Display.Height = 600.0f;
		}

		// The world's `StarterGui`, which is where a collector has to live to
		// draw at all.
		//
		// **Made by the fixture rather than by each case.** Containment is a
		// rule every case is now subject to, and parenting by hand in thirty of
		// them would be thirty chances to forget — and a forgotten one lays out
		// to nothing, which reads as the case being wrong rather than the
		// fixture being incomplete.
		//
		// Named rather than classed, for `Layout.cpp`'s reason: `StarterGui` is
		// `scene`'s service and this module may not link `scene`, so the string
		// is duplicated here and pinned by a test.
		Entity Container;

		Entity Make(std::string_view klass, Entity parent = Entity{}) {
			const Entity made = Data.CreateInstance(GuiClass(klass), klass);

			if (parent != engine::ecs::NULL_ENTITY) {
				Data.SetParent(made, parent);
			} else if (Data.IsA(made, GuiClass("LayerCollector"))) {
				// An unparented collector draws nothing, so a case that did not
				// say where its `ScreenGui` lives gets the ordinary answer
				// rather than the degenerate one.
				Data.SetParent(made, Container);
			}

			return made;
		}

		void Box(Entity instance, float x, float y, float width, float height) {
			Element element;
			element.Position = UDim2{0.0f, x, 0.0f, y};
			element.Size = UDim2{0.0f, width, 0.0f, height};
			Data.Set(instance, element);
		}

		void Compile() {
			Request.Hovered = Route.Hovered();
			Request.Pressed = Route.Pressed();
			List.Rebuild(Data, Request);
		}

		std::vector<GuiEvent> Move(float x, float y, bool down = false) {
			Compile();
			Pointer pointer;
			pointer.Position = Vector2{x, y};
			pointer.Down = down;
			const std::span<const GuiEvent> events = Route.Update(Data, List.Commands(), pointer);
			return std::vector<GuiEvent>(events.begin(), events.end());
		}

		bool Has(const std::vector<GuiEvent> &events, EventKind kind, Entity instance) {
			for (const GuiEvent &event : events) {
				if (event.Kind == kind && event.Instance == instance) {
					return true;
				}
			}
			return false;
		}
	};
}

TEST_CASE("a button takes the click and a plain frame does not", "[gui][input]") {
	World world("gui_input.active");
	const Entity screen = world.Make("ScreenGui");
	const Entity panel = world.Make("Frame", screen);
	const Entity button = world.Make("TextButton", panel);

	world.Box(panel, 0.0f, 0.0f, 400.0f, 400.0f);
	world.Box(button, 10.0f, 10.0f, 100.0f, 40.0f);

	world.Compile();

	// A `GuiButton` is hit whatever its `Active` says, which is Roblox's rule.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{50.0f, 30.0f}) == button);

	// **A decorative panel is transparent to input.** This is what lets a
	// background exist without swallowing the interface it contains, and it is
	// why the pick walk continues past a miss rather than stopping.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{300.0f, 300.0f}) == NULL_ENTITY);

	Element active;
	active.Position = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	active.Size = UDim2{0.0f, 400.0f, 0.0f, 400.0f};
	active.Active = true;
	world.Data.Set(panel, active);

	world.Compile();
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{300.0f, 300.0f}) == panel);
}

TEST_CASE("the topmost element wins", "[gui][input]") {
	World world("gui_input.order");
	const Entity screen = world.Make("ScreenGui");
	const Entity under = world.Make("TextButton", screen);
	const Entity over = world.Make("TextButton", screen);

	world.Box(under, 0.0f, 0.0f, 200.0f, 200.0f);
	world.Box(over, 0.0f, 0.0f, 200.0f, 200.0f);

	world.Compile();

	// The list is in paint order and the pick reads it backwards, so the answer
	// falls out of the same sort that decided what covered what. Two answers to
	// "what is on top" is the thing this arrangement exists to prevent.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{50.0f, 50.0f}) == over);
}

TEST_CASE("a clipped element is not hit where it would have been", "[gui][input]") {
	World world("gui_input.clip");
	const Entity screen = world.Make("ScreenGui");
	const Entity window = world.Make("Frame", screen);
	const Entity button = world.Make("TextButton", window);

	Element outer;
	outer.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	outer.ClipsDescendants = true;
	world.Data.Set(window, outer);

	world.Box(button, 0.0f, 0.0f, 400.0f, 400.0f);
	world.Compile();

	CHECK(Pick(world.Data, world.List.Commands(), Vector2{50.0f, 50.0f}) == button);

	// **Inside its rectangle and outside its clip.** The rectangle survives
	// deliberately — `Resolved` keeps it so "scrolled away" is
	// distinguishable from "never laid out" — so the clip test is what stops
	// the hit.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{250.0f, 250.0f}) == NULL_ENTITY);
}

TEST_CASE("enter and leave fire in that order", "[gui][input]") {
	World world("gui_input.hover");
	const Entity screen = world.Make("ScreenGui");
	const Entity left = world.Make("TextButton", screen);
	const Entity right = world.Make("TextButton", screen);

	world.Box(left, 0.0f, 0.0f, 100.0f, 100.0f);
	world.Box(right, 100.0f, 0.0f, 100.0f, 100.0f);

	std::vector<GuiEvent> events = world.Move(50.0f, 50.0f);
	CHECK(world.Has(events, EventKind::MouseEnter, left));
	CHECK(world.Route.Hovered() == left);

	events = world.Move(150.0f, 50.0f);

	// **Leave before enter.** A handler that moves something on leave has to run
	// before the one reacting to the arrival, or a swap between two adjacent
	// buttons produces an enter against state the leave is about to undo.
	REQUIRE(events.size() >= 2);
	CHECK(events[0].Kind == EventKind::MouseLeave);
	CHECK(events[0].Instance == left);
	CHECK(events[1].Kind == EventKind::MouseEnter);
	CHECK(events[1].Instance == right);
}

TEST_CASE("the first update does not invent a move", "[gui][input]") {
	World world("gui_input.first");
	const Entity screen = world.Make("ScreenGui");
	const Entity corner = world.Make("TextButton", screen);
	world.Box(corner, 0.0f, 0.0f, 50.0f, 50.0f);

	// A router that has never seen a pointer must not report a move from the
	// origin, which would fire at whatever happens to be under (0, 0) — here,
	// deliberately, a button.
	const std::vector<GuiEvent> events = world.Move(0.0f, 0.0f);
	CHECK_FALSE(world.Has(events, EventKind::MouseMoved, corner));
	CHECK(world.Has(events, EventKind::MouseEnter, corner));
}

TEST_CASE("press and release on one element activates it", "[gui][input]") {
	World world("gui_input.activate");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);
	world.Box(button, 0.0f, 0.0f, 100.0f, 100.0f);

	world.Move(50.0f, 50.0f);

	std::vector<GuiEvent> events = world.Move(50.0f, 50.0f, true);
	CHECK(world.Has(events, EventKind::InputBegan, button));
	CHECK(world.Route.Pressed() == button);

	events = world.Move(50.0f, 50.0f, false);
	CHECK(world.Has(events, EventKind::InputEnded, button));
	CHECK(world.Has(events, EventKind::Activated, button));
	CHECK(world.Route.Pressed() == NULL_ENTITY);
}

TEST_CASE("dragging off a button ends the input and activates nothing", "[gui][input]") {
	World world("gui_input.drag");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);
	const Entity other = world.Make("TextButton", screen);

	world.Box(button, 0.0f, 0.0f, 100.0f, 100.0f);
	world.Box(other, 200.0f, 0.0f, 100.0f, 100.0f);

	world.Move(50.0f, 50.0f);
	world.Move(50.0f, 50.0f, true);

	const std::vector<GuiEvent> events = world.Move(250.0f, 50.0f, false);

	// **`InputEnded` goes where the press began, not where the release
	// happened.** That is what makes dragging off a button and back one
	// interaction; a release routed by position leaves the pressed button stuck
	// looking pressed.
	CHECK(world.Has(events, EventKind::InputEnded, button));
	CHECK_FALSE(world.Has(events, EventKind::Activated, button));
	CHECK_FALSE(world.Has(events, EventKind::Activated, other));
}

TEST_CASE("dragging off and back still activates", "[gui][input]") {
	World world("gui_input.return");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);
	world.Box(button, 0.0f, 0.0f, 100.0f, 100.0f);

	world.Move(50.0f, 50.0f);
	world.Move(50.0f, 50.0f, true);
	world.Move(500.0f, 500.0f, true);

	const std::vector<GuiEvent> events = world.Move(50.0f, 50.0f, false);
	CHECK(world.Has(events, EventKind::Activated, button));
}

TEST_CASE("leaving the canvas ends the hover", "[gui][input]") {
	World world("gui_input.outside");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);
	world.Box(button, 0.0f, 0.0f, 100.0f, 100.0f);

	world.Move(50.0f, 50.0f);
	REQUIRE(world.Route.Hovered() == button);

	world.Compile();
	Pointer gone;
	gone.Position = Vector2{50.0f, 50.0f};
	gone.Inside = false;

	// Position unchanged, and the hover still ends: the pointer being over the
	// canvas at all is a separate fact from where it is, and a window the mouse
	// has left is exactly the case a position test cannot see.
	const std::span<const GuiEvent> events = world.Route.Update(world.Data, world.List.Commands(), gone);
	REQUIRE(!events.empty());
	CHECK(events[0].Kind == EventKind::MouseLeave);
	CHECK(world.Route.Hovered() == NULL_ENTITY);
}

TEST_CASE("an invisible button is not hit", "[gui][input]") {
	World world("gui_input.hidden");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	element.Visible = false;
	world.Data.Set(button, element);

	world.Compile();
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{50.0f, 50.0f}) == NULL_ENTITY);
}

TEST_CASE("every element under a point comes back front to back", "[gui][input]") {
	// **The three things `Pick` cannot answer**, which is why `ElementsAt` exists:
	// what is under the pointer *as well as* the thing that takes the click, in
	// what order, and only within one player's container.
	//
	// Two overlapping frames, so the order is actually asserted rather than
	// "something came back", and the assertion is made twice with the `ZIndex`
	// swapped — a walk that happened to return tree order would pass the first
	// half and fail the second.
	World world("gui_input.elements");

	const Entity mine = world.Make("ScreenGui");
	const Entity under = world.Make("Frame", mine);
	const Entity over = world.Make("Frame", mine);
	const Entity elsewhere = world.Make("Frame", mine);

	// **A second container, which is a player's own `PlayerGui` beside the
	// `StarterGui` template the fixture makes.** The layout resolves every
	// collector in the world — `Layout.hpp` names both containers — so an
	// unscoped answer would hand one player the rectangles of the template and of
	// everybody else's copy, which is the failure `ResetPlayerGui` exists one door
	// along to prevent.
	const Entity theirs =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "PlayerGui");
	const Entity intruder = world.Make("Frame", world.Make("ScreenGui", theirs));

	Element top;
	top.Position = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	top.Size = UDim2{0.0f, 200.0f, 0.0f, 200.0f};
	top.ZIndex = 5;
	world.Data.Set(over, top);

	world.Box(under, 0.0f, 0.0f, 200.0f, 200.0f);
	world.Box(elsewhere, 400.0f, 400.0f, 100.0f, 100.0f);
	world.Box(intruder, 0.0f, 0.0f, 200.0f, 200.0f);
	world.Compile();

	std::vector<Entity> found;
	CHECK(ElementsAt(world.Data, mine, Vector2{50.0f, 50.0f}, found) == 2);
	REQUIRE(found.size() == 2);

	// Front to back, so the higher `ZIndex` leads — and neither answer is the
	// frame in the other container or the one nowhere near the point.
	CHECK(found[0] == over);
	CHECK(found[1] == under);
	CHECK(found[0] != intruder);
	CHECK(found[1] != elsewhere);

	// **A plain `Frame` is in the list where `Pick` refuses it**, which is the
	// whole difference between the two questions: a decorative panel is
	// transparent to a click and is still under the pointer.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{50.0f, 50.0f}) == NULL_ENTITY);

	// The same point, the order swapped by the property that decides it.
	top.ZIndex = -5;
	world.Data.Set(over, top);
	world.Compile();

	REQUIRE(ElementsAt(world.Data, mine, Vector2{50.0f, 50.0f}, found) == 2);
	CHECK(found[0] == under);
	CHECK(found[1] == over);

	// The other container answers about its own, and a point over nothing is an
	// empty list rather than a stale one.
	REQUIRE(ElementsAt(world.Data, theirs, Vector2{50.0f, 50.0f}, found) == 1);
	CHECK(found[0] == intruder);

	CHECK(ElementsAt(world.Data, mine, Vector2{700.0f, 50.0f}, found) == 0);
	CHECK(found.empty());
	CHECK(ElementsAt(world.Data, NULL_ENTITY, Vector2{50.0f, 50.0f}, found) == 0);
}

TEST_CASE("a hidden or clipped element is not under the point either", "[gui][input]") {
	// `ElementsAt` drops the `Active` test and keeps every other one, and this is
	// where that is pinned: the two filters below are what make the answer "what
	// is on screen here" rather than "what has a rectangle here".
	World world("gui_input.elements.hidden");

	const Entity screen = world.Make("ScreenGui");
	const Entity window = world.Make("Frame", screen);
	const Entity inside = world.Make("Frame", window);
	const Entity hidden = world.Make("Frame", screen);

	Element outer;
	outer.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	outer.ClipsDescendants = true;
	world.Data.Set(window, outer);

	Element invisible;
	invisible.Size = UDim2{0.0f, 400.0f, 0.0f, 400.0f};
	invisible.Visible = false;
	world.Data.Set(hidden, invisible);

	world.Box(inside, 0.0f, 0.0f, 400.0f, 400.0f);
	world.Compile();

	std::vector<Entity> found;

	// Inside the clip: the child and the window it is in, child first.
	REQUIRE(ElementsAt(world.Data, screen, Vector2{50.0f, 50.0f}, found) == 2);
	CHECK(found[0] == inside);
	CHECK(found[1] == window);

	// Inside the child's rectangle and outside its clip. The rectangle survives
	// deliberately, so the clip test is what stops it — and the invisible frame
	// covering the same point is absent for the other reason.
	CHECK(ElementsAt(world.Data, screen, Vector2{250.0f, 250.0f}, found) == 0);
}

TEST_CASE("a rotated element is clickable where it is drawn", "[gui][input]") {
	// **`D00025`, closed, and it is the half no backend could fix.** `Pick`
	// tested `DrawCommand::Bounds` as an axis-aligned rectangle and ignored
	// `Rotation` beside it, so a rotated button drew in one place and answered a
	// pointer in another — the kind of bug people file twice, once against the
	// drawing and once against the input.
	//
	// A wide, short button turned a quarter turn is tall and narrow. The two
	// points below are chosen so that *each* is inside exactly one of the two
	// interpretations: a hit test that ignored the rotation gets both answers
	// backwards, which is the strongest form this check takes.
	World world("gui_input.rotated");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	Element element;
	element.Position = UDim2{0.0f, 100.0f, 0.0f, 190.0f};
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 20.0f};
	element.Rotation = 90.0f;
	world.Data.Set(button, element);

	world.Compile();

	// The unrotated box spans x 100..300, y 190..210 — centred at (200, 200).
	// Turned, it spans x 190..210, y 100..300.
	const Vector2 alongTurned{200.0f, 120.0f}; // inside turned, outside flat
	const Vector2 alongFlat{280.0f, 200.0f};   // inside flat, outside turned

	CHECK(Pick(world.Data, world.List.Commands(), alongTurned) == button);
	CHECK(Pick(world.Data, world.List.Commands(), alongFlat) == NULL_ENTITY);

	// **The centre is in both**, so it is asserted separately rather than being
	// allowed to stand in for either — a test that only checked the middle would
	// pass against a hit test that ignored rotation entirely.
	CHECK(Pick(world.Data, world.List.Commands(), Vector2{200.0f, 200.0f}) == button);
}

// --- the keyboard focus -----------------------------------------------------
//
// **What the router decides, where `gui/tests/Services.cpp` covers where the
// answer rests.** Both halves are needed: the storage cases would pass against a
// router that never called `Focus`, and these would pass against a `Focus` that
// stored nothing if they only looked at the events.

namespace {
	// A `TextBox` with a rectangle, which is what a pointer can be aimed at.
	Entity MakeTextBox(World &world, Entity parent, float x, float y) {
		const Entity made = world.Make("TextBox", parent);
		world.Box(made, x, y, 100.0f, 40.0f);
		return made;
	}
}

TEST_CASE("a press takes the keyboard and a press elsewhere gives it back", "[gui][input]") {
	World world("gui_input.focus");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = MakeTextBox(world, screen, 0.0f, 0.0f);
	const Entity button = world.Make("TextButton", screen);
	world.Box(button, 0.0f, 100.0f, 100.0f, 40.0f);

	// Arriving over the box takes nothing: a hover is not a decision about
	// where typing goes.
	std::vector<GuiEvent> events = world.Move(50.0f, 20.0f);
	CHECK_FALSE(world.Has(events, EventKind::Focused, box));
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);

	events = world.Move(50.0f, 20.0f, true);
	CHECK(world.Has(events, EventKind::InputBegan, box));
	CHECK(world.Has(events, EventKind::Focused, box));
	CHECK(FocusedTextBox(world.Data) == box);

	// **The release does not give it back**, which is what makes dragging a
	// selection out of a box and letting go somewhere else keep it focused.
	events = world.Move(50.0f, 120.0f, false);
	CHECK_FALSE(world.Has(events, EventKind::FocusReleased, box));
	CHECK(FocusedTextBox(world.Data) == box);

	// A press on the button does.
	events = world.Move(50.0f, 120.0f, true);
	CHECK(world.Has(events, EventKind::FocusReleased, box));
	CHECK_FALSE(world.Has(events, EventKind::Focused, button));
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);
}

TEST_CASE("a press that lands on nothing releases the keyboard", "[gui][input]") {
	// **Roblox's answer, and the one a person expects**: clicking the background
	// is how anybody stops typing. Keeping focus until some *other* box took it
	// would leave a game with no way to give the keyboard back to itself without
	// adding a widget for the purpose.
	World world("gui_input.focus_background");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = MakeTextBox(world, screen, 0.0f, 0.0f);

	world.Move(50.0f, 20.0f, true);
	REQUIRE(FocusedTextBox(world.Data) == box);
	world.Move(50.0f, 20.0f, false);

	const std::vector<GuiEvent> events = world.Move(600.0f, 500.0f, true);
	CHECK(world.Has(events, EventKind::FocusReleased, box));
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);
}

TEST_CASE("the focus moves between two boxes as one pair of events", "[gui][input]") {
	// **Released before captured**, for the reason `MouseLeave` precedes
	// `MouseEnter`: a handler putting state back runs before the one reacting to
	// the arrival. A world in which two boxes both read as focused never exists.
	World world("gui_input.focus_swap");
	const Entity screen = world.Make("ScreenGui");
	const Entity first = MakeTextBox(world, screen, 0.0f, 0.0f);
	const Entity second = MakeTextBox(world, screen, 0.0f, 100.0f);

	world.Move(50.0f, 20.0f, true);
	world.Move(50.0f, 20.0f, false);
	REQUIRE(FocusedTextBox(world.Data) == first);

	const std::vector<GuiEvent> events = world.Move(50.0f, 120.0f, true);
	CHECK(FocusedTextBox(world.Data) == second);

	size_t released = events.size();
	size_t focused = events.size();
	for (size_t index = 0; index < events.size(); index++) {
		if (events[index].Kind == EventKind::FocusReleased && events[index].Instance == first) {
			released = index;
		}
		if (events[index].Kind == EventKind::Focused && events[index].Instance == second) {
			focused = index;
		}
	}

	REQUIRE(released < events.size());
	REQUIRE(focused < events.size());
	CHECK(released < focused);
}

TEST_CASE("a destroyed focused box releases nothing and blocks nothing", "[gui][input]") {
	// **The dangling case from the router's side.** `FocusReleased` names an
	// element and a dead element has nothing to fire at — `Router::Forget`'s
	// argument — so the event is not emitted; what must still be true is that the
	// next press focuses, which a stale handle compared against itself would
	// refuse.
	World world("gui_input.focus_destroyed");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = MakeTextBox(world, screen, 0.0f, 0.0f);
	const Entity other = MakeTextBox(world, screen, 0.0f, 100.0f);

	world.Move(50.0f, 20.0f, true);
	REQUIRE(FocusedTextBox(world.Data) == box);
	world.Move(50.0f, 20.0f, false);

	world.Data.DestroyInstance(box);
	CHECK(FocusedTextBox(world.Data) == NULL_ENTITY);

	const std::vector<GuiEvent> events = world.Move(50.0f, 120.0f, true);
	for (const GuiEvent &event : events) {
		CHECK(event.Kind != EventKind::FocusReleased);
	}
	CHECK(FocusedTextBox(world.Data) == other);
}
