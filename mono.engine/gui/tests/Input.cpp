#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
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
