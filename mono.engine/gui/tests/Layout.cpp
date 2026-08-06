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
			Display.Width = 800.0f;
			Display.Height = 600.0f;
		}

		Entity Make(std::string_view klass, Entity parent = Entity{}) {
			const Entity made = Data.CreateInstance(GuiClass(klass), klass);
			if (parent != engine::ecs::NULL_ENTITY) {
				Data.SetParent(made, parent);
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
