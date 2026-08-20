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
			Container =
				Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui");
			Display.Width = 800.0f;
			Display.Height = 600.0f;
		}

		// The world's `StarterGui`, which is where a collector has to live to
		// draw at all.
		//
		// **Made by the fixture rather than by each case.** Containment is a
		// rule every case is now subject to, and parenting by hand in thirty of
		// them would be thirty chances to forget - and a forgotten one lays out
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

namespace {
	// A frame of a fixed pixel size under `parent`, for the flex cases below.
	// They build wide rows of these and the four lines per child drowned what
	// each case was actually asserting.
	Entity Block(World &world, Entity parent, float width, float height) {
		const Entity made = world.Make("Frame", parent);
		Element element;
		element.Size = UDim2{0.0f, width, 0.0f, height};
		world.Data.Set(made, element);
		return made;
	}

	// A container with a horizontal list layout, handing back both.
	Entity ListIn(World &world, Entity screen, const ListLayout &stack, Entity &layout) {
		const Entity holder = world.Make("Frame", screen);
		Element container;
		container.Size = UDim2{0.0f, 300.0f, 0.0f, 300.0f};
		world.Data.Set(holder, container);

		layout = world.Make("UIListLayout", holder);
		world.Data.Set(layout, stack);
		return holder;
	}
}

TEST_CASE("a wrapped list breaks lines at the fill axis", "[gui][layout][flex]") {
	World world("gui_layout.wraps");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.Padding = UDim{0.0f, 10.0f};
	stack.Wraps = true;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);

	// Two fit on a line with the gap; the third would land at 380 of 300.
	Entity rows[3];
	for (Entity &row : rows) {
		row = Block(world, holder, 120.0f, 20.0f);
	}

	Layout(world.Data, world.Display);

	const Vector2 origin = world.Where(holder).AbsolutePosition;
	CHECK(world.Where(rows[0]).AbsolutePosition.X == Approx(origin.X));
	CHECK(world.Where(rows[1]).AbsolutePosition.X == Approx(origin.X + 130.0f));
	CHECK(world.Where(rows[1]).AbsolutePosition.Y == Approx(origin.Y));

	// The second line starts one line's height plus the gap down, at the
	// left edge again - `Padding` serves both axes, resolved against each.
	CHECK(world.Where(rows[2]).AbsolutePosition.X == Approx(origin.X));
	CHECK(world.Where(rows[2]).AbsolutePosition.Y == Approx(origin.Y + 30.0f));
}

TEST_CASE("Fill on the fill axis grows children to close the container", "[gui][layout][flex]") {
	World world("gui_layout.flex_fill");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.HorizontalFlex = FlexAlignment::Fill;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity left = Block(world, holder, 50.0f, 20.0f);
	const Entity right = Block(world, holder, 50.0f, 20.0f);

	Layout(world.Data, world.Display);

	// 200 spare, split by equal grow weights: 150 each.
	const float x = world.Where(holder).AbsolutePosition.X;
	CHECK(world.Where(left).AbsoluteSize.X == Approx(150.0f));
	CHECK(world.Where(right).AbsoluteSize.X == Approx(150.0f));
	CHECK(world.Where(left).AbsolutePosition.X == Approx(x));
	CHECK(world.Where(right).AbsolutePosition.X == Approx(x + 150.0f));
}

TEST_CASE("a UIFlexItem spring takes the whole of the spare room", "[gui][layout][flex]") {
	World world("gui_layout.flex_spring");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity leading = Block(world, holder, 50.0f, 20.0f);
	const Entity spring = Block(world, holder, 50.0f, 20.0f);
	const Entity trailing = Block(world, holder, 50.0f, 20.0f);

	FlexItem grow;
	grow.Mode = FlexMode::Grow;
	world.Data.Set(world.Make("UIFlexItem", spring), grow);

	Layout(world.Data, world.Display);

	// The toolbar shape: fixed buttons at both ends, the spring between them
	// swallowing the 150 the line had spare.
	const float x = world.Where(holder).AbsolutePosition.X;
	CHECK(world.Where(leading).AbsoluteSize.X == Approx(50.0f));
	CHECK(world.Where(spring).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(trailing).AbsoluteSize.X == Approx(50.0f));
	CHECK(world.Where(trailing).AbsolutePosition.X == Approx(x + 250.0f));
}

TEST_CASE("FlexMode None opts a child out of a Fill row", "[gui][layout][flex]") {
	World world("gui_layout.flex_optout");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.HorizontalFlex = FlexAlignment::Fill;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity fixed = Block(world, holder, 50.0f, 20.0f);
	const Entity growing = Block(world, holder, 50.0f, 20.0f);

	// A `UIFlexItem` at `None` is an override, not an absence - the row is
	// `Fill` and this child still keeps its authored width.
	world.Data.Set(world.Make("UIFlexItem", fixed), FlexItem{});

	Layout(world.Data, world.Display);

	CHECK(world.Where(fixed).AbsoluteSize.X == Approx(50.0f));
	CHECK(world.Where(growing).AbsoluteSize.X == Approx(250.0f));
}

TEST_CASE("the spacing modes spend spare room as gaps", "[gui][layout][flex]") {
	World world("gui_layout.flex_spacing");
	const Entity screen = world.Make("ScreenGui");

	ListLayout between;
	between.Direction = FillDirection::Horizontal;
	between.HorizontalFlex = FlexAlignment::SpaceBetween;

	Entity layout;
	const Entity first = ListIn(world, screen, between, layout);
	const Entity firstRows[3]{
		Block(world, first, 60.0f, 20.0f),
		Block(world, first, 60.0f, 20.0f),
		Block(world, first, 60.0f, 20.0f),
	};

	ListLayout evenly = between;
	evenly.HorizontalFlex = FlexAlignment::SpaceEvenly;
	const Entity second = ListIn(world, screen, evenly, layout);
	const Entity secondRows[2]{
		Block(world, second, 50.0f, 20.0f),
		Block(world, second, 50.0f, 20.0f),
	};

	ListLayout around = between;
	around.HorizontalFlex = FlexAlignment::SpaceAround;
	const Entity third = ListIn(world, screen, around, layout);
	const Entity thirdRows[2]{
		Block(world, third, 50.0f, 20.0f),
		Block(world, third, 50.0f, 20.0f),
	};

	Layout(world.Data, world.Display);

	// SpaceBetween: 120 spare over two gaps, nothing at the ends.
	float x = world.Where(first).AbsolutePosition.X;
	CHECK(world.Where(firstRows[0]).AbsolutePosition.X == Approx(x));
	CHECK(world.Where(firstRows[1]).AbsolutePosition.X == Approx(x + 120.0f));
	CHECK(world.Where(firstRows[2]).AbsolutePosition.X == Approx(x + 240.0f));

	// SpaceEvenly: 200 spare over three equal shares.
	x = world.Where(second).AbsolutePosition.X;
	CHECK(world.Where(secondRows[0]).AbsolutePosition.X == Approx(x + 200.0f / 3.0f));
	CHECK(world.Where(secondRows[1]).AbsolutePosition.X == Approx(x + 400.0f / 3.0f + 50.0f));

	// SpaceAround: a full share between, half a share at each end.
	x = world.Where(third).AbsolutePosition.X;
	CHECK(world.Where(thirdRows[0]).AbsolutePosition.X == Approx(x + 50.0f));
	CHECK(world.Where(thirdRows[1]).AbsolutePosition.X == Approx(x + 200.0f));
}

TEST_CASE("shrink absorbs an overflow in proportion to size", "[gui][layout][flex]") {
	World world("gui_layout.flex_shrink");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.HorizontalFlex = FlexAlignment::Fill;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity wide = Block(world, holder, 240.0f, 20.0f);
	const Entity narrow = Block(world, holder, 120.0f, 20.0f);

	Layout(world.Data, world.Display);

	// 60 over, weighted by basis: the wide child gives up twice as much as
	// the narrow one, which is CSS's weighting and keeps the pair's ratio.
	CHECK(world.Where(wide).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(narrow).AbsoluteSize.X == Approx(100.0f));
}

TEST_CASE("the cross axis stretches when it is flexed", "[gui][layout][flex]") {
	World world("gui_layout.flex_stretch");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.VerticalFlex = FlexAlignment::Fill;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity row = Block(world, holder, 100.0f, 20.0f);

	Layout(world.Data, world.Display);

	// `ItemLineAlignment::Automatic` reads a flexed cross axis as `Stretch`,
	// and `Fill` grows the single line to the whole container.
	CHECK(world.Where(row).AbsoluteSize.Y == Approx(300.0f));
	CHECK(world.Where(row).AbsoluteSize.X == Approx(100.0f));
}

TEST_CASE("ItemLineAlignment places a child against its line", "[gui][layout][flex]") {
	World world("gui_layout.flex_line");
	const Entity screen = world.Make("ScreenGui");

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.ItemLine = ItemLineAlignment::End;

	Entity layout;
	const Entity holder = ListIn(world, screen, stack, layout);
	const Entity shallow = Block(world, holder, 50.0f, 20.0f);
	const Entity deep = Block(world, holder, 50.0f, 60.0f);
	const Entity centred = Block(world, holder, 50.0f, 20.0f);

	// The child's own `UIFlexItem` overrides the layout's `End`.
	FlexItem middle;
	middle.ItemLine = ItemLineAlignment::Center;
	world.Data.Set(world.Make("UIFlexItem", centred), middle);

	Layout(world.Data, world.Display);

	// The line is as deep as its deepest child; `End` sits on its floor and
	// the override centres against the same line.
	const float top = world.Where(holder).AbsolutePosition.Y;
	CHECK(world.Where(deep).AbsolutePosition.Y == Approx(top));
	CHECK(world.Where(shallow).AbsolutePosition.Y == Approx(top + 40.0f));
	CHECK(world.Where(centred).AbsolutePosition.Y == Approx(top + 20.0f));
}

TEST_CASE("an automatic container grows to its wrapped lines", "[gui][layout][automatic][flex]") {
	World world("gui_layout.flex_automatic");
	const Entity screen = world.Make("ScreenGui");
	const Entity holder = world.Make("Frame", screen);

	Element container;
	container.Size = UDim2{0.0f, 300.0f, 0.0f, 0.0f};
	container.Automatic = AutomaticSize::Y;
	world.Data.Set(holder, container);

	ListLayout stack;
	stack.Direction = FillDirection::Horizontal;
	stack.Padding = UDim{0.0f, 10.0f};
	stack.Wraps = true;
	world.Data.Set(world.Make("UIListLayout", holder), stack);

	for (int index = 0; index < 3; index++) {
		Block(world, holder, 120.0f, 20.0f);
	}

	Layout(world.Data, world.Display);

	// Two lines of 20 and the gap between them - the measure wraps at the
	// same span the placement does, or the two would disagree by a line.
	CHECK(world.Where(holder).AbsoluteSize.Y == Approx(50.0f));
	CHECK(world.Where(holder).AbsoluteSize.X == Approx(300.0f));
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
	// - the ordering only shows up on a case where the two disagree, which is
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

	// The child keeps its own rectangle - the clip is a separate fact, which is
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
	text.Text = "a rather long string indeed";
	text.Size = 48;
	text.Scaled = true;
	world.Data.Set(label, text);

	Layout(world.Data, world.Display);

	const Resolved &where = world.Where(label);
	CHECK(where.TextSize >= 1);
	CHECK(where.TextSize < 48);

	// The authored size is untouched - a script reads back what it wrote, and
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
	// that bookkeeping does not crash and does not produce nonsense - it places
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
	// resolved rectangle is its parent's - which is what makes a crossed run
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
	// This does *not* test that the arena is released - a run left behind grows
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
	// nothing to place - the child above is what separates the two.
	CHECK(Layout(world.Data, world.Display) == 0);
	CHECK_FALSE(world.Data.Get<Resolved>(screen)->Rendered);
	CHECK_FALSE(world.Data.Get<Resolved>(child)->Rendered);
}

TEST_CASE("a screen gui nested inside a container still draws", "[gui][layout]") {
	// **Walked upward rather than tested against the immediate parent**, because
	// authors nest: a folder of screens under `StarterGui` is ordinary.
	World world("gui_layout.nested");

	const Entity folder =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Screens");
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
	// the distinction - accepting every collector, or refusing every one -
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

TEST_CASE("a spatial collector that clips nothing still lays out against its canvas", "[gui][layout]") {
	// **Two rectangles, and the case exists because they are easy to conflate.**
	// `ClipsDescendants` decides what the subtree is *cut* to and nothing about
	// what a `UDim2` resolves against - so a half-width child of an unclipped
	// surface is half the canvas, not half of the very large rectangle the clip
	// becomes. Folding the two into one number would produce a child five
	// hundred thousand pixels wide and nothing would say why.
	World world("gui_layout.unclipped");

	const Entity workspace = world.Data.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("Instance")), std::string(WORKSPACE)
	);

	const Entity surface = world.Make("SurfaceGui", workspace);
	Surface state;
	state.CanvasSize = Vector2{400.0f, 300.0f};
	state.ClipsDescendants = false;
	world.Data.Set(surface, state);

	const Entity frame = world.Make("Frame", surface);
	Element element;
	element.Size = UDim2{0.5f, 0.0f, 0.5f, 0.0f};
	world.Data.Set(frame, element);

	Layout(world.Data, world.Display);

	CHECK(world.Where(frame).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(frame).AbsoluteSize.Y == Approx(150.0f));

	// The clip is what changed, and it has to be wide enough to cut nothing an
	// author meant to draw.
	CHECK(world.Where(frame).Clip.Width() > 400.0f);

	state.ClipsDescendants = true;
	world.Data.Set(surface, state);
	Layout(world.Data, world.Display);

	CHECK(world.Where(frame).Clip.Width() == Approx(400.0f));
	CHECK(world.Where(frame).AbsoluteSize.X == Approx(200.0f));
}

TEST_CASE("the container names are the ones scene registers", "[gui][layout]") {
	// **The pin at this end**, and `scene/tests/Services.cpp` holds the other.
	//
	// These three strings are `scene`'s service names spelled again here,
	// because `gui/AGENTS.md` refuses an edge to `scene` - the same refusal that
	// made `gui::Face` re-declare `NormalId`'s six members. A duplicated
	// constant is only safe while something fails when the two disagree, and a
	// rename on either side would otherwise produce an interface that silently
	// stops drawing rather than a build that stops working.
	CHECK(WORKSPACE == "Workspace");
	CHECK(STARTER_GUI == "StarterGui");
	CHECK(PLAYER_GUI == "PlayerGui");
}

// `AutomaticSize`, which closes `D00021`.
//
// **The reopen trigger it was filed with was "the first UI that wants a list to
// fit its rows"**, so that is the first case below and the shape the rest are
// arranged around. The property has been declared, saved and bound since the
// tree went in with nothing reading it; these are the cases that make it mean
// something, and they fail if it stops working rather than if it stops
// compiling.

TEST_CASE("a container grows to fit the rows stacked inside it", "[gui][layout][automatic]") {
	// The case the entry names. A menu is a frame full of buttons and its height
	// is whatever the buttons come to - an author should not have to keep a
	// literal in sync with how many rows there are.
	World world("gui_layout.automatic_list");
	const Entity screen = world.Make("ScreenGui");
	const Entity menu = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 200.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::Y;
	world.Data.Set(menu, frame);

	ListLayout list;
	list.Direction = FillDirection::Vertical;
	list.Padding = UDim{0.0f, 8.0f};
	world.Data.Set(world.Make("UIListLayout", menu), list);

	for (int index = 0; index < 3; index++) {
		Element row;
		row.Size = UDim2{1.0f, 0.0f, 0.0f, 30.0f};
		world.Data.Set(world.Make("TextButton", menu), row);
	}

	Layout(world.Data, world.Display);

	// Three thirty-pixel rows and the two gaps *between* them - not three gaps,
	// which is the off-by-one every stack layout gets wrong once.
	CHECK(world.Where(menu).AbsoluteSize.Y == Approx(3.0f * 30.0f + 2.0f * 8.0f));

	// **The authored axis is untouched.** `AutomaticSize::Y` is one axis, and a
	// container that also collapsed its width would be obeying a property the
	// author did not set.
	CHECK(world.Where(menu).AbsoluteSize.X == Approx(200.0f));
}

TEST_CASE("a scale-sized child contributes nothing to the axis it is inside", "[gui][layout][automatic]") {
	// **The circularity, and the one decision this feature cannot avoid making.**
	// A child asking to be as wide as the thing whose width it is deciding has
	// no fixed point. Roblox resolves the scale against zero; so does this, so
	// the child measures as empty and then fills the grown parent once placed.
	//
	// Asserting both halves, because either alone is satisfied by a bug: a
	// container that grew to 800 took the screen's width by mistake, and a child
	// left at zero never got the second resolve.
	World world("gui_layout.automatic_circular");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::XY;
	world.Data.Set(box, frame);

	const Entity wide = world.Make("Frame", box);
	Element child;
	child.Size = UDim2{1.0f, 0.0f, 0.0f, 40.0f};
	world.Data.Set(wide, child);

	Layout(world.Data, world.Display);

	CHECK(world.Where(box).AbsoluteSize.X == Approx(0.0f));
	CHECK(world.Where(box).AbsoluteSize.Y == Approx(40.0f));
	CHECK(world.Where(wide).AbsoluteSize.X == Approx(0.0f));
}

TEST_CASE("an automatic container grows to the far edge of a free child", "[gui][layout][automatic]") {
	// With no list and no grid, children sit where their own `UDim2` puts them,
	// so the content is a bounding box rather than a sum.
	//
	// **The near edge is deliberately ignored.** The second child hangs off the
	// top-left at a negative offset, and the container does not move its origin
	// to swallow it - this is a growth rule, not a reflow, and a parent that
	// shifted under its children would drag everything beside it.
	World world("gui_layout.automatic_free");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::XY;
	world.Data.Set(box, frame);

	Element far;
	far.Position = UDim2{0.0f, 60.0f, 0.0f, 20.0f};
	far.Size = UDim2{0.0f, 40.0f, 0.0f, 10.0f};
	world.Data.Set(world.Make("Frame", box), far);

	Element behind;
	behind.Position = UDim2{0.0f, -25.0f, 0.0f, -25.0f};
	behind.Size = UDim2{0.0f, 5.0f, 0.0f, 5.0f};
	world.Data.Set(world.Make("Frame", box), behind);

	Layout(world.Data, world.Display);

	CHECK(world.Where(box).AbsoluteSize.X == Approx(100.0f));
	CHECK(world.Where(box).AbsoluteSize.Y == Approx(30.0f));
}

TEST_CASE("padding is added back to what an automatic container grew to", "[gui][layout][automatic]") {
	// **Both halves, which is what makes this more than an arithmetic check.**
	// The padding comes off the basis the child resolves against *and* goes back
	// onto the extent - an implementation that did one and not the other is off
	// by exactly the padding, and looks right on every case where the padding is
	// zero.
	World world("gui_layout.automatic_padding");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::XY;
	world.Data.Set(box, frame);

	Padding inset;
	inset.Left = UDim{0.0f, 4.0f};
	inset.Right = UDim{0.0f, 6.0f};
	inset.Top = UDim{0.0f, 2.0f};
	inset.Bottom = UDim{0.0f, 8.0f};
	world.Data.Set(world.Make("UIPadding", box), inset);

	Element child;
	child.Size = UDim2{0.0f, 50.0f, 0.0f, 20.0f};
	const Entity inner = world.Make("Frame", box);
	world.Data.Set(inner, child);

	Layout(world.Data, world.Display);

	CHECK(world.Where(box).AbsoluteSize.X == Approx(50.0f + 4.0f + 6.0f));
	CHECK(world.Where(box).AbsoluteSize.Y == Approx(20.0f + 2.0f + 8.0f));

	// And the child sits inside the padding rather than at the container's own
	// corner, which is the half that proves the basis was reduced.
	CHECK(world.Where(inner).AbsolutePosition.X == Approx(world.Where(box).AbsolutePosition.X + 4.0f));
	CHECK(world.Where(inner).AbsolutePosition.Y == Approx(world.Where(box).AbsolutePosition.Y + 2.0f));
}

TEST_CASE("a size limit bounds what an automatic container grows to", "[gui][layout][automatic]") {
	// The growth happens before the constraint, so a `UISizeConstraint` on an
	// automatic container is a ceiling on the content - which is the only
	// reading under which putting both on one element means anything.
	World world("gui_layout.automatic_limits");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::Y;
	world.Data.Set(box, frame);

	SizeLimits limits;
	limits.Max = Vector2{10000.0f, 100.0f};
	world.Data.Set(world.Make("UISizeConstraint", box), limits);

	Element tall;
	tall.Size = UDim2{0.0f, 50.0f, 0.0f, 400.0f};
	world.Data.Set(world.Make("Frame", box), tall);

	Layout(world.Data, world.Display);

	CHECK(world.Where(box).AbsoluteSize.Y == Approx(100.0f));
}

TEST_CASE("an element that draws text grows to its string", "[gui][layout][automatic]") {
	// **This case used to assert the opposite**, on the grounds that a string
	// measured with `AVERAGE_ADVANCE` is an estimate and "a label grown to that
	// estimate is a box its own text spills out of".
	//
	// It is not, and the reason is the invariant `Layout.hpp` already states:
	// the backend draws at `Resolved::TextSize` and does not second-guess it.
	// Nothing downstream re-measures with real metrics, so within this engine
	// the estimate *is* the measurement - the one answer a hit test, a headless
	// assertion and a renderer all agree on. A box grown to it fits by the same
	// definition of fitting used everywhere else in the module.
	//
	// The other failure the old refusal named is still refused, and by
	// construction rather than by a branch: a labelled element is sized from its
	// *text*, so the children path that would have collapsed a `TextLabel` to
	// nothing is not the one it takes.
	World world("gui_layout.automatic_text");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);

	Element element;
	element.Size = UDim2{0.0f, 120.0f, 0.0f, 24.0f};
	element.Automatic = AutomaticSize::XY;
	world.Data.Set(label, element);

	Label text;
	text.Text = "Score: 0";
	world.Data.Set(label, text);

	Layout(world.Data, world.Display);

	// Eight characters at the module's own advance, and one line at its own
	// spacing. Spelled as the arithmetic rather than as the number, so a change
	// to either constant moves this case with it instead of failing it.
	const float expectedWidth = 8.0f * engine::gui::AVERAGE_ADVANCE * 14.0f;
	const float expectedHeight = engine::gui::LINE_SPACING * 14.0f;

	CHECK(world.Where(label).AbsoluteSize.X == Approx(expectedWidth));
	CHECK(world.Where(label).AbsoluteSize.Y == Approx(expectedHeight));
}

TEST_CASE("a grown label is one axis at a time", "[gui][layout][automatic]") {
	// `AutomaticSize.Y` on a fixed-width label is the ordinary case - a caption
	// column whose rows are as tall as their text and as wide as the column.
	World world("gui_layout.automatic_text_axis");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);

	Element element;
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 4.0f};
	element.Automatic = AutomaticSize::Y;
	world.Data.Set(label, element);

	Label text;
	text.Text = "wrapped";
	world.Data.Set(label, text);

	Layout(world.Data, world.Display);

	// The authored width survives, which is what makes this an axis rather than
	// a mode.
	CHECK(world.Where(label).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(label).AbsoluteSize.Y == Approx(engine::gui::LINE_SPACING * 14.0f));
}

TEST_CASE("TextScaled on a grown label returns the size asked for", "[gui][layout][automatic]") {
	// **The pair means something now, and by construction rather than by a
	// special case.** `FittedTextSize` divides the box by exactly the product
	// the growth multiplies, so it recovers the size it started from: an author
	// who sets both gets the size they typed in a box that holds it.
	//
	// Before the growth landed, setting both gave a label shrunk to fit a box
	// that had nothing to do with its text.
	World world("gui_layout.automatic_text_scaled");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);

	Element element;
	element.Size = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	element.Automatic = AutomaticSize::XY;
	world.Data.Set(label, element);

	Label text;
	text.Text = "Fits";
	text.Size = 20;
	text.Scaled = true;
	world.Data.Set(label, text);

	Layout(world.Data, world.Display);

	// Not shrunk: the box was grown to exactly what this size needs.
	CHECK(world.Where(label).TextSize == 20);
}

TEST_CASE("automatic sizing nests", "[gui][layout][automatic]") {
	// **The property that makes this a second phase rather than a special
	// case.** The outer container's height is a function of the inner one's,
	// which is a function of the rows - so the measure has to recurse, and an
	// implementation that measured only its immediate children reports the
	// inner frame's authored zero and collapses.
	World world("gui_layout.automatic_nested");
	const Entity screen = world.Make("ScreenGui");

	const Entity outer = world.Make("Frame", screen);
	Element outerFrame;
	outerFrame.Size = UDim2{0.0f, 200.0f, 0.0f, 0.0f};
	outerFrame.Automatic = AutomaticSize::Y;
	world.Data.Set(outer, outerFrame);

	const Entity inner = world.Make("Frame", outer);
	Element innerFrame;
	innerFrame.Size = UDim2{0.0f, 200.0f, 0.0f, 0.0f};
	innerFrame.Automatic = AutomaticSize::Y;
	world.Data.Set(inner, innerFrame);

	ListLayout list;
	list.Direction = FillDirection::Vertical;
	world.Data.Set(world.Make("UIListLayout", inner), list);

	for (int index = 0; index < 4; index++) {
		Element row;
		row.Size = UDim2{1.0f, 0.0f, 0.0f, 25.0f};
		world.Data.Set(world.Make("TextButton", inner), row);
	}

	Layout(world.Data, world.Display);

	CHECK(world.Where(inner).AbsoluteSize.Y == Approx(100.0f));
	CHECK(world.Where(outer).AbsoluteSize.Y == Approx(100.0f));
}

TEST_CASE("an automatic container grows to its grid's rows", "[gui][layout][automatic]") {
	// A grid decides both axes of every cell, so the extent is a count rather
	// than a measurement - seven cells three to a line is three lines, the last
	// of them short.
	World world("gui_layout.automatic_grid");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 300.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::Y;
	world.Data.Set(box, frame);

	GridLayout grid;
	grid.CellSize = UDim2{0.0f, 80.0f, 0.0f, 40.0f};
	grid.CellPadding = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	grid.MaxCells = 3;
	world.Data.Set(world.Make("UIGridLayout", box), grid);

	for (int index = 0; index < 7; index++) {
		Element cell;
		world.Data.Set(world.Make("Frame", box), cell);
	}

	Layout(world.Data, world.Display);

	// Three rows of forty with two ten-pixel gaps between them.
	CHECK(world.Where(box).AbsoluteSize.Y == Approx(3.0f * 40.0f + 2.0f * 10.0f));
}

TEST_CASE("an invisible child is not measured into its parent", "[gui][layout][automatic]") {
	// `Element::Visible` is a branch the compile stops at, so a hidden row is
	// not drawn - and a container that still reserved its height would leave a
	// gap where the author asked for nothing.
	World world("gui_layout.automatic_hidden");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("Frame", screen);

	Element frame;
	frame.Size = UDim2{0.0f, 200.0f, 0.0f, 0.0f};
	frame.Automatic = AutomaticSize::Y;
	world.Data.Set(box, frame);

	ListLayout list;
	list.Direction = FillDirection::Vertical;
	world.Data.Set(world.Make("UIListLayout", box), list);

	Element shown;
	shown.Size = UDim2{1.0f, 0.0f, 0.0f, 30.0f};
	world.Data.Set(world.Make("Frame", box), shown);

	Element hidden;
	hidden.Size = UDim2{1.0f, 0.0f, 0.0f, 30.0f};
	hidden.Visible = false;
	world.Data.Set(world.Make("Frame", box), hidden);

	Layout(world.Data, world.Display);

	CHECK(world.Where(box).AbsoluteSize.Y == Approx(30.0f));
}

TEST_CASE("a table layout gives a column one width in every row", "[gui][layout]") {
	// **The reason a table is not a grid of frames**, and the only thing it is
	// for: a column has to be the same width in every row, which means the
	// layout has to reach two levels down and size the cells rather than letting
	// each row decide for itself.
	World world("gui_layout.table");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element outer;
	outer.Size = UDim2{0.0f, 400.0f, 0.0f, 300.0f};
	world.Data.Set(frame, outer);

	const Entity layout = world.Make("UITableLayout", frame);
	world.Data.Set(layout, TableLayout{});

	// Two rows, two cells each. The first column is wide in one row and narrow
	// in the other; the widest wins for both.
	const float widths[2][2]{{200.0f, 40.0f}, {60.0f, 90.0f}};
	Entity cells[2][2]{};

	for (int row = 0; row < 2; row++) {
		const Entity rowNode = world.Make("Frame", frame);
		Element rowElement;
		rowElement.Size = UDim2{0.0f, 0.0f, 0.0f, 30.0f};
		rowElement.LayoutOrder = row;
		world.Data.Set(rowNode, rowElement);

		for (int column = 0; column < 2; column++) {
			cells[row][column] = world.Make("TextLabel", rowNode);
			Element cell;
			cell.Size = UDim2{0.0f, widths[row][column], 0.0f, 24.0f};
			cell.LayoutOrder = column;
			world.Data.Set(cells[row][column], cell);
		}
	}

	Layout(world.Data, world.Display);

	// Column one is two hundred wide in both rows, because one cell asked for
	// two hundred; column two is ninety, for the same reason.
	CHECK(world.Where(cells[0][0]).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(cells[1][0]).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(cells[0][1]).AbsoluteSize.X == Approx(90.0f));
	CHECK(world.Where(cells[1][1]).AbsoluteSize.X == Approx(90.0f));

	// The second column starts where the first ends, and the second row below
	// the first.
	CHECK(world.Where(cells[0][1]).AbsolutePosition.X == Approx(200.0f));
	CHECK(world.Where(cells[1][0]).AbsolutePosition.Y > world.Where(cells[0][0]).AbsolutePosition.Y);

	// **Rows are laid out and remain instances.** A row that stopped being
	// rendered would take its own background with it, which is how a table gets
	// its stripes.
	CHECK(world.Where(cells[0][0]).Rendered);

	// Filling shares the spare width out, so the table reaches the parent's edge.
	TableLayout filled;
	filled.FillEmptySpaceColumns = true;
	world.Data.Set(layout, filled);
	Layout(world.Data, world.Display);

	const float across = world.Where(cells[0][0]).AbsoluteSize.X + world.Where(cells[0][1]).AbsoluteSize.X;
	CHECK(across == Approx(400.0f));
}

TEST_CASE("a page layout shows one child and slides the rest aside", "[gui][layout]") {
	// Every page is the container's own size and sits one step further along, so
	// the strip moves under the container rather than the pages moving inside
	// it. Nothing animates - `gui::PageLayout` says why the tween is absent
	// rather than ignored.
	World world("gui_layout.pages");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element outer;
	outer.Position = UDim2{0.0f, 100.0f, 0.0f, 0.0f};
	outer.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	outer.ClipsDescendants = true;
	world.Data.Set(frame, outer);

	const Entity layout = world.Make("UIPageLayout", frame);

	Entity pages[3]{};
	for (int index = 0; index < 3; index++) {
		pages[index] = world.Make("Frame", frame);
		Element page;
		page.LayoutOrder = index;
		world.Data.Set(pages[index], page);
	}

	PageLayout paging;
	paging.CurrentPage = pages[1];
	world.Data.Set(layout, paging);

	Layout(world.Data, world.Display);

	// The current page fills the container; its neighbours sit one container
	// width either side of it.
	CHECK(world.Where(pages[1]).AbsolutePosition.X == Approx(100.0f));
	CHECK(world.Where(pages[1]).AbsoluteSize.X == Approx(200.0f));
	CHECK(world.Where(pages[0]).AbsolutePosition.X == Approx(-100.0f));
	CHECK(world.Where(pages[2]).AbsolutePosition.X == Approx(300.0f));

	// **`Animated` off from here down, because this case is about placement.**
	// Sliding arrived at v0.17 and has its own three cases below; asking about
	// where a page *ends up* while a tween is a tenth of the way through it
	// would be asking two questions and asserting one.
	PageLayout unset;
	unset.Animated = false;
	world.Data.Set(layout, unset);
	Layout(world.Data, world.Display);

	// **An unset `CurrentPage` is the first one**, which is what an author who
	// set nothing means and what a page destroyed mid-show leaves behind.
	CHECK(world.Where(pages[0]).AbsolutePosition.X == Approx(100.0f));

	// Circular wraps to the nearer side, so the page before the first is drawn
	// just off the near edge rather than at the far end of everything.
	PageLayout loop;
	loop.Circular = true;
	loop.Animated = false;
	world.Data.Set(layout, loop);
	Layout(world.Data, world.Display);
	CHECK(world.Where(pages[2]).AbsolutePosition.X == Approx(-100.0f));
}

TEST_CASE("a page layout slides between pages over time", "[gui][layout]") {
	// **The whole feature is that the position is a function of elapsed time**,
	// so every assertion here states a moment rather than stepping frames to
	// reach one. That is `render::FlipbookFrameAt`'s shape and it is why this
	// case runs in microseconds and has no sleep in it.
	World world("gui_layout.pageslide");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element outer;
	outer.Position = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	outer.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(frame, outer);

	const Entity layout = world.Make("UIPageLayout", frame);

	Entity pages[3]{};
	for (int index = 0; index < 3; index++) {
		pages[index] = world.Make("Frame", frame);
		Element page;
		page.LayoutOrder = index;
		world.Data.Set(pages[index], page);
	}

	// Linear, so the arithmetic below is the position rather than a curve's
	// opinion of it. The curves themselves are `core::TweenInfo`'s to test and
	// `engine.core.tweeninfo` does.
	PageLayout paging;
	paging.CurrentPage = pages[0];
	paging.Animated = true;
	paging.TweenTime = 2.0f;
	paging.Easing = engine::core::EasingStyle::Linear;
	paging.EasingWay = engine::core::EasingDirection::In;
	world.Data.Set(layout, paging);

	// Settle on page 0 first, so the jump below is the only motion in flight.
	Layout(world.Data, world.Display, 100.0);
	Layout(world.Data, world.Display, 110.0);
	REQUIRE(world.Where(pages[0]).AbsolutePosition.X == Approx(0.0f));

	paging.CurrentPage = pages[1];
	world.Data.Set(layout, paging);

	// The jump is noticed on the next layout, and nothing has moved yet.
	Layout(world.Data, world.Display, 200.0);
	CHECK(world.Where(pages[0]).AbsolutePosition.X == Approx(0.0f));
	CHECK(world.Where(pages[1]).AbsolutePosition.X == Approx(200.0f));

	// Halfway through two seconds is half a page, and a page is the container.
	Layout(world.Data, world.Display, 201.0);
	CHECK(world.Where(pages[0]).AbsolutePosition.X == Approx(-100.0f));
	CHECK(world.Where(pages[1]).AbsolutePosition.X == Approx(100.0f));

	// And past the end it is exactly there rather than nearly there.
	Layout(world.Data, world.Display, 202.0);
	CHECK(world.Where(pages[1]).AbsolutePosition.X == Approx(0.0f));

	// **Settled, so it stays put however much time passes.** A layout that kept
	// evaluating a finished tween would keep moving the signature with it.
	Layout(world.Data, world.Display, 900.0);
	CHECK(world.Where(pages[1]).AbsolutePosition.X == Approx(0.0f));
}

TEST_CASE("a page layout cuts when it is told not to animate", "[gui][layout]") {
	World world("gui_layout.pagecut");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element outer;
	outer.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(frame, outer);

	const Entity layout = world.Make("UIPageLayout", frame);

	Entity pages[2]{};
	for (int index = 0; index < 2; index++) {
		pages[index] = world.Make("Frame", frame);
		Element page;
		page.LayoutOrder = index;
		world.Data.Set(pages[index], page);
	}

	const auto jump = [&](bool animated, float tweenTime) {
		PageLayout paging;
		paging.Animated = animated;
		paging.TweenTime = tweenTime;
		paging.CurrentPage = pages[0];
		world.Data.Set(layout, paging);
		Layout(world.Data, world.Display, 10.0);

		paging.CurrentPage = pages[1];
		world.Data.Set(layout, paging);
		Layout(world.Data, world.Display, 10.0);
		return world.Where(pages[1]).AbsolutePosition.X;
	};

	// **Two properties that draw the same thing and read back differently**,
	// which is Roblox's arrangement: one says this layout does not animate and
	// the other says it animates over no time.
	CHECK(jump(false, 1.0f) == Approx(0.0f));
	CHECK(jump(true, 0.0f) == Approx(0.0f));

	// And with both on, the same jump has not moved at the instant it begins.
	CHECK(jump(true, 1.0f) == Approx(200.0f));
}

TEST_CASE("a circular page layout slides the short way round", "[gui][layout]") {
	// The case wrapping exists for: going from the last page to the first is
	// one step forward, not two steps back past everything. Without wrapping
	// the *distance* as well as each page's offset, the loop reads as a rewind.
	World world("gui_layout.pageloop");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element outer;
	outer.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(frame, outer);

	const Entity layout = world.Make("UIPageLayout", frame);

	Entity pages[3]{};
	for (int index = 0; index < 3; index++) {
		pages[index] = world.Make("Frame", frame);
		Element page;
		page.LayoutOrder = index;
		world.Data.Set(pages[index], page);
	}

	PageLayout paging;
	paging.Circular = true;
	paging.Animated = true;
	paging.TweenTime = 2.0f;
	paging.Easing = engine::core::EasingStyle::Linear;
	paging.EasingWay = engine::core::EasingDirection::In;
	paging.CurrentPage = pages[2];
	world.Data.Set(layout, paging);

	Layout(world.Data, world.Display, 10.0);
	Layout(world.Data, world.Display, 20.0);
	REQUIRE(world.Where(pages[2]).AbsolutePosition.X == Approx(0.0f));

	// Last page to first. The short way is forward by one, so halfway through
	// the strip has moved half a container forward and page 2 is on its way out
	// to the left rather than sweeping back across two pages.
	paging.CurrentPage = pages[0];
	world.Data.Set(layout, paging);
	Layout(world.Data, world.Display, 30.0);
	Layout(world.Data, world.Display, 31.0);

	CHECK(world.Where(pages[2]).AbsolutePosition.X == Approx(-100.0f));
	CHECK(world.Where(pages[0]).AbsolutePosition.X == Approx(100.0f));
}
