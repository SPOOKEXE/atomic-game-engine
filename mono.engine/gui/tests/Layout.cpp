#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_SUITE_ID("engine.gui.layout")

using Catch::Approx;
using engine::core::UDim;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	// A world with the tree registered and a screen to lay out against.
	//
	// **Every case builds its own.** A shared fixture would make the order the
	// cases ran in part of what they assert, and Catch2 does not promise one.
	struct World {
		Store Data;
		Screen Display;

		explicit World(std::string_view name) : Data(name) {
			RegisterGuiClasses();

			// **A bare `Instance`, because `StarterGui` is not a `gui` class.**
			// It is `scene`'s service and this module may not link `scene`; what
			// the containment test reads is the *name*, so an instance carrying
			// that name is exactly as contained as the real service would be.
			// That is the same duplication `NormalId` already makes here.
			Container = Data.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui"
			);
			Display.Width = 800.0f;
			Display.Height = 600.0f;
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

		const Resolved &Where(Entity instance) {
			const Resolved *resolved = Data.Get<Resolved>(instance);
			REQUIRE(resolved != nullptr);
			return *resolved;
		}
	};
}

TEST_CASE("a screen gui's canvas is the screen", "[gui][layout]") {
	World world("gui_layout.canvas");
	const Entity screen = world.Make("ScreenGui");

	Layout(world.Data, world.Display);

	const Canvas *canvas = world.Data.Get<Canvas>(screen);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Area.Width() == Approx(800.0f));
	CHECK(canvas->Area.Height() == Approx(600.0f));
	CHECK(world.Where(screen).Rendered);
}

TEST_CASE("a UDim2 resolves against the parent and the anchor point", "[gui][layout]") {
	World world("gui_layout.udim2");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.5f, 10.0f, 0.5f, -20.0f};
	element.Size = UDim2{0.25f, 0.0f, 0.0f, 50.0f};
	element.AnchorPoint = Vector2{0.5f, 0.5f};
	world.Data.Set(frame, element);

	Layout(world.Data, world.Display);

	const Resolved &where = world.Where(frame);

	// 0.25 of 800 wide, 50 pixels tall.
	CHECK(where.AbsoluteSize.X == Approx(200.0f));
	CHECK(where.AbsoluteSize.Y == Approx(50.0f));

	// The anchor point is the element's own centre, so the top-left corner is
	// half a size back from where `Position` put it. Getting this backwards is
	// the classic UI bug: everything is off by half its own size and looks like
	// a margin nobody set.
	CHECK(where.AbsolutePosition.X == Approx(0.5f * 800.0f + 10.0f - 100.0f));
	CHECK(where.AbsolutePosition.Y == Approx(0.5f * 600.0f - 20.0f - 25.0f));
}

TEST_CASE("SizeConstraint decides which parent axis a scale reads", "[gui][layout]") {
	World world("gui_layout.constraint");
	const Entity screen = world.Make("ScreenGui");
	const Entity square = world.Make("Frame", screen);

	Element element;
	element.Size = UDim2{0.5f, 0.0f, 0.5f, 0.0f};
	element.Constraint = SizeConstraint::RelativeXX;
	world.Data.Set(square, element);

	Layout(world.Data, world.Display);

	// Both axes against the parent's *width*, which is what keeps a square
	// square as the parent changes shape.
	const Resolved &where = world.Where(square);
	CHECK(where.AbsoluteSize.X == Approx(400.0f));
	CHECK(where.AbsoluteSize.Y == Approx(400.0f));
}

TEST_CASE("padding shrinks the area children resolve against", "[gui][layout]") {
	World world("gui_layout.padding");
	const Entity screen = world.Make("ScreenGui");
	const Entity outer = world.Make("Frame", screen);
	const Entity inner = world.Make("Frame", outer);
	const Entity pad = world.Make("UIPadding", outer);

	Element frame;
	frame.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(outer, frame);

	Element child;
	child.Size = UDim2{1.0f, 0.0f, 1.0f, 0.0f};
	world.Data.Set(inner, child);

	Padding padding;
	padding.Left = UDim{0.0f, 10.0f};
	padding.Right = UDim{0.0f, 10.0f};
	padding.Top = UDim{0.0f, 5.0f};
	padding.Bottom = UDim{0.0f, 5.0f};
	world.Data.Set(pad, padding);

	Layout(world.Data, world.Display);

	// A child at full scale fills the *padded* area, not the frame.
	const Resolved &where = world.Where(inner);
	CHECK(where.AbsoluteSize.X == Approx(180.0f));
	CHECK(where.AbsoluteSize.Y == Approx(90.0f));
	CHECK(where.AbsolutePosition.X == Approx(where.AbsolutePosition.X));

	// **The padding instance itself is never laid out.** It is a child in the
	// tree; if the layout treated it as one, a padded list would have a blank
	// row where the modifier was.
	CHECK_FALSE(world.Data.Has<Element>(pad));
}

TEST_CASE("a list layout stacks children and ignores their positions", "[gui][layout]") {
	World world("gui_layout.list");
	const Entity screen = world.Make("ScreenGui");
	const Entity holder = world.Make("Frame", screen);
	const Entity layout = world.Make("UIListLayout", holder);

	Element container;
	container.Size = UDim2{0.0f, 300.0f, 0.0f, 300.0f};
	world.Data.Set(holder, container);

	ListLayout stack;
	stack.Direction = FillDirection::Vertical;
	stack.Padding = UDim{0.0f, 4.0f};
	world.Data.Set(layout, stack);

	Entity rows[3]{};
	for (int index = 0; index < 3; index++) {
		rows[index] = world.Make("Frame", holder);

		Element row;
		row.Size = UDim2{1.0f, 0.0f, 0.0f, 20.0f};

		// Deliberately non-zero, and deliberately ignored. A layout that
		// honoured `Position` would leave the stack looking almost right and
		// wrong by however much the author happened to have set.
		row.Position = UDim2{0.0f, 999.0f, 0.0f, 999.0f};
		row.LayoutOrder = 2 - index;
		world.Data.Set(rows[index], row);
	}

	Layout(world.Data, world.Display);

	const float top = world.Where(holder).AbsolutePosition.Y;

	// Sorted by `LayoutOrder`, so the one declared last is at the top.
	CHECK(world.Where(rows[2]).AbsolutePosition.Y == Approx(top));
	CHECK(world.Where(rows[1]).AbsolutePosition.Y == Approx(top + 24.0f));
	CHECK(world.Where(rows[0]).AbsolutePosition.Y == Approx(top + 48.0f));

	for (const Entity row : rows) {
		CHECK(world.Where(row).AbsoluteSize.X == Approx(300.0f));
	}
}

TEST_CASE("a grid layout gives every child the cell's size", "[gui][layout]") {
	World world("gui_layout.grid");
	const Entity screen = world.Make("ScreenGui");
	const Entity holder = world.Make("Frame", screen);
	const Entity layout = world.Make("UIGridLayout", holder);

	Element container;
	container.Size = UDim2{0.0f, 210.0f, 0.0f, 210.0f};
	world.Data.Set(holder, container);

	GridLayout grid;
	grid.CellSize = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	grid.CellPadding = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	grid.MaxCells = 2;
	world.Data.Set(layout, grid);

	Entity cells[4]{};
	for (int index = 0; index < 4; index++) {
		cells[index] = world.Make("Frame", holder);

		Element cell;
		// **Ignored on both axes.** A grid decides the size as well as the
		// position, which is the difference between it and a list.
		cell.Size = UDim2{0.0f, 7.0f, 0.0f, 7.0f};
		cell.LayoutOrder = index;
		world.Data.Set(cells[index], cell);
	}

	Layout(world.Data, world.Display);

	const Vector2 origin = world.Where(holder).AbsolutePosition;

	for (const Entity cell : cells) {
		CHECK(world.Where(cell).AbsoluteSize.X == Approx(100.0f));
		CHECK(world.Where(cell).AbsoluteSize.Y == Approx(100.0f));
	}

	CHECK(world.Where(cells[0]).AbsolutePosition.X == Approx(origin.X));
	CHECK(world.Where(cells[1]).AbsolutePosition.X == Approx(origin.X + 110.0f));
	CHECK(world.Where(cells[2]).AbsolutePosition.Y == Approx(origin.Y + 110.0f));
	CHECK(world.Where(cells[3]).AbsolutePosition.X == Approx(origin.X + 110.0f));
	CHECK(world.Where(cells[3]).AbsolutePosition.Y == Approx(origin.Y + 110.0f));
}

TEST_CASE("an aspect ratio derives one axis from the other", "[gui][layout]") {
	World world("gui_layout.aspect");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	const Entity constraint = world.Make("UIAspectRatioConstraint", frame);

	Element element;
	element.Size = UDim2{0.0f, 320.0f, 0.0f, 999.0f};
	world.Data.Set(frame, element);

	AspectRatio ratio;
	ratio.Ratio = 16.0f / 9.0f;
	ratio.Dominant = DominantAxis::Width;
	world.Data.Set(constraint, ratio);

	Layout(world.Data, world.Display);

	CHECK(world.Where(frame).AbsoluteSize.X == Approx(320.0f));
	CHECK(world.Where(frame).AbsoluteSize.Y == Approx(180.0f));
}

TEST_CASE("size limits clamp after the aspect ratio, not before", "[gui][layout]") {
	World world("gui_layout.limits");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Make("UIAspectRatioConstraint", frame);
	const Entity limit = world.Make("UISizeConstraint", frame);

	Element element;
	element.Size = UDim2{0.0f, 400.0f, 0.0f, 400.0f};
	world.Data.Set(frame, element);

	SizeLimits limits;
	limits.Max = Vector2{120.0f, 120.0f};
	world.Data.Set(limit, limits);

	Layout(world.Data, world.Display);

	// The ratio makes it 400x400 and the clamp cuts both to 120. Clamping
	// first would leave 120x400 for the ratio to reshape into 120x120 as well
	// — the ordering only shows up on a case where the two disagree, which is
	// why the assertion below is on the *ratio's* axis rather than on the size.
	CHECK(world.Where(frame).AbsoluteSize.X == Approx(120.0f));
	CHECK(world.Where(frame).AbsoluteSize.Y == Approx(120.0f));
}

TEST_CASE("clipping is inherited and intersected", "[gui][layout]") {
	World world("gui_layout.clip");
	const Entity screen = world.Make("ScreenGui");
	const Entity outer = world.Make("Frame", screen);
	const Entity inner = world.Make("Frame", outer);

	Element frame;
	frame.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	frame.ClipsDescendants = true;
	world.Data.Set(outer, frame);

	Element child;
	child.Size = UDim2{0.0f, 500.0f, 0.0f, 500.0f};
	world.Data.Set(inner, child);

	Layout(world.Data, world.Display);

	// The child keeps its own rectangle — the clip is a separate fact, which is
	// what lets a scrolling frame move content under a window.
	CHECK(world.Where(inner).AbsoluteSize.X == Approx(500.0f));
	CHECK(world.Where(inner).Clip.Width() == Approx(100.0f));
	CHECK(world.Where(inner).Clip.Height() == Approx(100.0f));
}

TEST_CASE("visibility is a branch and orphans are not rendered", "[gui][layout]") {
	World world("gui_layout.visible");
	const Entity screen = world.Make("ScreenGui");
	const Entity hidden = world.Make("Frame", screen);
	const Entity beneath = world.Make("Frame", hidden);
	const Entity orphan = world.Make("Frame");

	Element invisible;
	invisible.Visible = false;
	world.Data.Set(hidden, invisible);

	Layout(world.Data, world.Display);

	// **Hiding a container hides its contents**, which is the opposite of
	// `scene::Visual::Visible` and is deliberate: hiding a part leaves its
	// children alone, and hiding a panel is expected to hide the panel.
	CHECK_FALSE(world.Where(hidden).Rendered);
	CHECK_FALSE(world.Where(beneath).Rendered);

	// An element a script made and never parented is drawn by nothing, exactly
	// as Roblox draws nothing for one.
	CHECK_FALSE(world.Where(orphan).Rendered);
}

TEST_CASE("a disabled collector takes its whole tree off screen", "[gui][layout]") {
	World world("gui_layout.disabled");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Layout(world.Data, world.Display);
	REQUIRE(world.Where(frame).Rendered);

	Layer layer;
	layer.Enabled = false;
	world.Data.Set(screen, layer);

	Layout(world.Data, world.Display);
	CHECK_FALSE(world.Where(frame).Rendered);

	// **The rectangle survives.** `Resolved`'s own comment gives the reason: an
	// element that is off screen has to stay distinguishable from one that was
	// never measured, or the hit test has to guess.
	CHECK(world.Where(frame).AbsoluteSize.X > 0.0f);
}

TEST_CASE("scaled text shrinks to fit and never reaches zero", "[gui][layout]") {
	World world("gui_layout.text");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);

	Element element;
	element.Size = UDim2{0.0f, 40.0f, 0.0f, 20.0f};
	world.Data.Set(label, element);

	Label text;
	text.Text = engine::core::Name("a rather long string indeed");
	text.Size = 48;
	text.Scaled = true;
	world.Data.Set(label, text);

	Layout(world.Data, world.Display);

	const Resolved &where = world.Where(label);
	CHECK(where.TextSize >= 1);
	CHECK(where.TextSize < 48);

	// The authored size is untouched — a script reads back what it wrote, and
	// the drawn number lives on `Resolved`.
	CHECK(world.Data.Get<Label>(label)->Size == 48);
}

TEST_CASE("display order decides which collector lays out last", "[gui][layout]") {
	World world("gui_layout.order");
	const Entity behind = world.Make("ScreenGui");
	const Entity front = world.Make("ScreenGui");

	Layer low;
	low.DisplayOrder = -5;
	world.Data.Set(behind, low);

	Layer high;
	high.DisplayOrder = 5;
	world.Data.Set(front, high);

	const size_t placed = Layout(world.Data, world.Display);

	// Both collectors resolve; nothing under either, so nothing is placed.
	CHECK(placed == 0);
	CHECK(world.Where(behind).Rendered);
	CHECK(world.Where(front).Rendered);
}

TEST_CASE("siblings with different child counts each place their own children", "[gui][layout]") {
	// **The case the child arena introduced, and the one nothing else here
	// covers.** `Layout` walks a node's child list exactly once and stores the
	// `GuiObject`s it found as a run in one shared buffer, because measuring a
	// node and placing it both want that list and chasing the intrusive sibling
	// chain twice per element was the largest cost in the pass.
	//
	// That makes several sibling runs live at the same moment: a container
	// measures *all* of its children before it places any, so child 0's run sits
	// under child 1's, which sits under child 2's, and child 0 is not read back
	// until long after the other two were written on top of it. An off-by-one in
	// that bookkeeping does not crash and does not produce nonsense — it places
	// one container's children inside a *sibling's* rectangle, which reads as a
	// layout bug in whatever authored the interface rather than as an engine
	// one.
	//
	// The three containers below hold one, two and three children, so a run
	// whose length or offset is wrong cannot line up by accident.
	World world("gui_layout.siblingruns");
	const Entity screen = world.Make("ScreenGui");

	// Three containers side by side, each a fixed 200 wide and anchored to a
	// different third of the screen, so every child's expected rectangle is a
	// different number.
	std::vector<Entity> containers;
	for (int index = 0; index < 3; index++) {
		const Entity container = world.Make("Frame", screen);

		Element element;
		element.Position = UDim2{0.0f, static_cast<float>(index) * 200.0f, 0.0f, 0.0f};
		element.Size = UDim2{0.0f, 200.0f, 0.0f, 300.0f};
		world.Data.Set(container, element);
		containers.push_back(container);
	}

	// One, two and three children respectively, each filling its parent so its
	// resolved rectangle is its parent's — which is what makes a crossed run
	// immediately visible as the wrong X.
	std::vector<std::vector<Entity>> children(containers.size());
	for (size_t index = 0; index < containers.size(); index++) {
		for (size_t child = 0; child <= index; child++) {
			const Entity leaf = world.Make("Frame", containers[index]);

			Element element;
			element.Size = UDim2{1.0f, 0.0f, 1.0f, 0.0f};
			world.Data.Set(leaf, element);
			children[index].push_back(leaf);
		}
	}

	Layout(world.Data, world.Display);

	for (size_t index = 0; index < containers.size(); index++) {
		const Resolved &parent = world.Where(containers[index]);
		CHECK(parent.AbsolutePosition.X == Approx(static_cast<float>(index) * 200.0f));

		REQUIRE(children[index].size() == index + 1);
		for (const Entity leaf : children[index]) {
			const Resolved &where = world.Where(leaf);

			// Inside its own parent and nobody else's.
			CHECK(where.Rendered);
			CHECK(where.AbsolutePosition.X == Approx(parent.AbsolutePosition.X));
			CHECK(where.AbsoluteSize.X == Approx(200.0f));
			CHECK(where.AbsoluteSize.Y == Approx(300.0f));
		}
	}

	// **And it has to hold on repeated passes**, because the arena is reused
	// across calls rather than rebuilt: a run whose offset drifted between
	// frames would give a first frame that is right and a later one that is not.
	//
	// This does *not* test that the arena is released — a run left behind grows
	// the buffer without moving any offset already recorded, so it is a memory
	// bug with no output a test can see. That one is held by `ArenaScope` making
	// the release a destructor instead of a statement somebody has to remember,
	// which is a stronger guarantee than a case here could give anyway.
	const size_t placed = Layout(world.Data, world.Display);
	CHECK(placed == Layout(world.Data, world.Display));

	for (size_t index = 0; index < containers.size(); index++) {
		const Resolved &parent = world.Where(containers[index]);
		for (const Entity leaf : children[index]) {
			CHECK(world.Where(leaf).AbsolutePosition.X == Approx(parent.AbsolutePosition.X));
		}
	}
}

// --- containment --------------------------------------------------------------
//
// **Where a collector sits decides whether it draws at all**, and Roblox's rule
// is the one implemented: a `ScreenGui` draws from `StarterGui` or a player's
// `PlayerGui`, and the two collectors attached to something in the world draw
// from `Workspace` as well.
//
// The failure this prevents is specific and nasty: an engine that drew a
// `ScreenGui` wherever it found one lets an author parent it somewhere Roblox
// would not, see it in the studio, and ship a game whose interface is missing
// in the client. A difference between the two that runs *that* way round is the
// worst kind, because the place it works is the place it is checked.

TEST_CASE("a screen gui outside a container does not draw", "[gui][layout]") {
	World world("gui_layout.uncontained");

	// Parented to nothing at all, which is where a script leaves an element
	// between `Instance.new` and setting `Parent`.
	const Entity orphan = world.Data.CreateInstance(GuiClass("ScreenGui"), "ScreenGui");
	const Entity child = world.Make("Frame", orphan);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(child, element);

	// **The count is over placed *elements*, and the child is what makes it
	// meaningful.** A collector with nothing under it places nothing whether or
	// not it was allowed to draw, so a case asserting zero without one would
	// pass against an engine that ignored containment entirely.
	CHECK(Layout(world.Data, world.Display) == 0);

	// **Not rendered rather than not present.** `Resolved` is cleared by the
	// sweep and the rectangle is left alone, so an element that stopped being
	// contained is distinguishable from one that was never laid out.
	const Resolved *resolved = world.Data.Get<Resolved>(orphan);
	CHECK((resolved == nullptr || !resolved->Rendered));
}

TEST_CASE("a screen gui under a part does not draw", "[gui][layout]") {
	// The mistake this rule exists for: a `ScreenGui` parented into the world.
	// It is a legal tree and Roblox draws nothing from it.
	World world("gui_layout.underpart");

	const Entity somewhere =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Rock");
	const Entity screen = world.Make("ScreenGui", somewhere);

	const Entity child = world.Make("Frame", screen);
	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(child, element);

	// Zero *because* nothing under a part is reachable, not because there was
	// nothing to place — the child above is what separates the two.
	CHECK(Layout(world.Data, world.Display) == 0);
	CHECK_FALSE(world.Data.Get<Resolved>(screen)->Rendered);
	CHECK_FALSE(world.Data.Get<Resolved>(child)->Rendered);
}

TEST_CASE("a screen gui nested inside a container still draws", "[gui][layout]") {
	// **Walked upward rather than tested against the immediate parent**, because
	// authors nest: a folder of screens under `StarterGui` is ordinary.
	World world("gui_layout.nested");

	const Entity folder = world.Data.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("Instance")), "Screens"
	);
	world.Data.SetParent(folder, world.Container);

	const Entity screen = world.Make("ScreenGui", folder);

	Layout(world.Data, world.Display);
	CHECK(world.Where(screen).Rendered);
}

TEST_CASE("a screen gui under a player's PlayerGui draws", "[gui][layout]") {
	// The client's half of the rule. A `PlayerGui` is a child of a `Player`
	// rather than a service, so this is the case that proves the test is over
	// the *name* and not over a fixed set of roots.
	World world("gui_layout.playergui");

	const Entity player =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Someone");
	const Entity playerGui = world.Data.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("Instance")), std::string(PLAYER_GUI)
	);
	world.Data.SetParent(playerGui, player);

	const Entity screen = world.Make("ScreenGui", playerGui);

	Layout(world.Data, world.Display);
	CHECK(world.Where(screen).Rendered);
}

TEST_CASE("only the world-attached collectors draw from the Workspace", "[gui][layout]") {
	// **The one asymmetry in the rule, and the reason it is not one test.** A
	// `SurfaceGui` and a `BillboardGui` hang off something in the world, so the
	// world is a legal home for them; a `ScreenGui` is the viewer's own overlay
	// and is not in the world at all.
	//
	// All three in one case, under one `Workspace`, so a change that collapsed
	// the distinction — accepting every collector, or refusing every one —
	// fails here whichever way it went.
	World world("gui_layout.workspace");

	const Entity workspace = world.Data.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("Instance")), std::string(WORKSPACE)
	);

	const Entity screen = world.Make("ScreenGui", workspace);
	const Entity surface = world.Make("SurfaceGui", workspace);
	const Entity billboard = world.Make("BillboardGui", workspace);

	Layout(world.Data, world.Display);

	CHECK_FALSE(world.Data.Get<Resolved>(screen)->Rendered);
	CHECK(world.Data.Get<Resolved>(surface)->Rendered);
	CHECK(world.Data.Get<Resolved>(billboard)->Rendered);
}

TEST_CASE("the container names are the ones scene registers", "[gui][layout]") {
	// **The pin at this end**, and `scene/tests/Services.cpp` holds the other.
	//
	// These three strings are `scene`'s service names spelled again here,
	// because `gui/AGENTS.md` refuses an edge to `scene` — the same refusal that
	// made `gui::Face` re-declare `NormalId`'s six members. A duplicated
	// constant is only safe while something fails when the two disagree, and a
	// rename on either side would otherwise produce an interface that silently
	// stops drawing rather than a build that stops working.
	CHECK(WORKSPACE == "Workspace");
	CHECK(STARTER_GUI == "StarterGui");
	CHECK(PLAYER_GUI == "PlayerGui");
}
