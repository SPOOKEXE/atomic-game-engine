#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.compile")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::EnumTable;
using engine::ecs::PropertyDescriptor;
using engine::ecs::PropertyType;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	struct World {
		Store Data;
		CompileRequest Request;
		Compiled List;

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

		bool Rebuild() {
			return List.Rebuild(Data, Request);
		}
	};

	// Changes a property to something it was not, whatever type it is.
	//
	// **The point of this is that it is generic.** `Compile.hpp` promises the
	// signature covers every field that reaches a rectangle, and the only way
	// to check a promise about *every* field is to walk the class table rather
	// than to list them again - a list would go stale in exactly the same way
	// the fold would.
	//
	// @return `false` for a property this cannot meaningfully perturb, which
	//         the caller skips rather than failing on.
	bool Perturb(Store &store, Entity instance, const PropertyDescriptor &property) {
		if (!property.Writable) {
			return false;
		}

		// A reference is a handle into this world and changing one means
		// reparenting, which the tree tests already cover and which would move
		// the element out from under this one.
		if (property.Type == PropertyType::Reference || property.Type == PropertyType::Opaque) {
			return false;
		}

		if (property.Type == PropertyType::Enum) {
			const std::vector<Name> members = EnumTable::MembersOf(property.EnumName);
			if (members.size() < 2) {
				return false;
			}

			Name current;
			store.GetProperty(instance, property.Name, &current, sizeof(current));
			const Name next = members[0] == current ? members[1] : members[0];
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		if (property.Type == PropertyType::Name) {
			const Name next("a value nothing else here uses");
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		// **Before the byte buffer below, and this is the branch whose absence
		// crashed rather than failing.** A `PropertyType::String` setter takes a
		// live `std::string` and assigns from it; handing it a span of zeroed
		// bytes makes it read a length and a pointer out of nothing. Every
		// production caller takes the same exception for the same reason, and
		// this one exists because the walk above reaches *every* declared
		// property - which is the whole point of the case.
		if (property.Type == PropertyType::String) {
			const std::string next = "a value nothing else here uses";
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		if (property.Type == PropertyType::Bool) {
			bool current = false;
			store.GetProperty(instance, property.Name, &current, sizeof(current));
			current = !current;
			return store.SetProperty(instance, property.Name, &current, sizeof(current));
		}

		// Everything left is one or more numbers laid end to end - an `int32`,
		// a `float`, a `Vector2`, a `UDim2`, a `Rect`, a `Color3`. Nudging the
		// first component is enough to say the field is folded in, and reading
		// the current value first means the nudge lands somewhere legal for a
		// property with a clamp on it.
		std::array<std::byte, 64> bytes{};
		if (property.Size > bytes.size()) {
			return false;
		}
		if (!store.GetProperty(instance, property.Name, bytes.data(), property.Size)) {
			return false;
		}

		if (property.Type == PropertyType::Int32 || property.Type == PropertyType::Int64) {
			int32_t value = 0;
			std::memcpy(&value, bytes.data(), sizeof(value));
			value += 3;
			std::memcpy(bytes.data(), &value, sizeof(value));
		} else {
			float value = 0.0f;
			std::memcpy(&value, bytes.data(), sizeof(value));

			// Toward the middle of the unit range rather than away from it, so
			// a transparency or a colour channel with a clamp on it still
			// lands somewhere different from where it started.
			value = value > 0.5f ? value - 0.25f : value + 0.25f;
			std::memcpy(bytes.data(), &value, sizeof(value));
		}

		return store.SetProperty(instance, property.Name, bytes.data(), property.Size);
	}
}

TEST_CASE("a still world compiles once and is then kept", "[gui][compile]") {
	World world("gui_compile.stable");
	const Entity screen = world.Make("ScreenGui");
	world.Make("Frame", screen);

	CHECK(world.Rebuild());
	CHECK(world.List.Rebuilds() == 1);

	// The whole point of the class. Ten frames of a UI nobody touched cost ten
	// scans and one compile.
	for (int frame = 0; frame < 10; frame++) {
		CHECK_FALSE(world.Rebuild());
	}

	CHECK(world.List.Rebuilds() == 1);
	CHECK(world.List.Requests() == 11);
}

TEST_CASE("every declared property moves the signature", "[gui][compile]") {
	// **This is the check that makes `Compile.hpp`'s table a contract.** A
	// field added to a component and not to the fold is a UI one edit stale,
	// and the symptom - a panel that updates on the *next* unrelated change -
	// is close to undebuggable from the outside.
	//
	// Every concrete class, every property it declares, one at a time.
	static constexpr std::string_view SUBJECTS[] = {
		"Frame",
		"CanvasGroup",
		"ScrollingFrame",
		"TextButton",
		"ImageButton",
		"TextLabel",
		"ImageLabel",
		"TextBox",
		"ViewportFrame",
		"UIListLayout",
		"UIGridLayout",
		"UIAspectRatioConstraint",
		"UISizeConstraint",
		"UITextSizeConstraint",
		"UIPadding",
		"UICorner",
		"UIStroke",
		"UIScale",
		"UIFlexItem",
	};

	size_t checked = 0;

	for (const std::string_view klass : SUBJECTS) {
		World world("gui_compile.fields");
		const Entity screen = world.Make("ScreenGui");
		const Entity host = world.Make("Frame", screen);
		const Entity subject = world.Make(klass, host);

		REQUIRE(world.Rebuild());

		for (const PropertyDescriptor &property : Classes::Describe(GuiClass(klass)).Properties) {
			// `Name` is `ecs`'s and reaches the compile only through
			// `SortOrder::Name`, which is folded separately; `Parent` is the
			// tree. Both are covered by their own cases below.
			if (property.Name == Name("Name") || property.Name == Name("Parent")) {
				continue;
			}

			const uint64_t before = world.List.Signature();
			if (!Perturb(world.Data, subject, property)) {
				continue;
			}

			INFO(klass << "." << property.Name.Text());
			world.Rebuild();
			CHECK(world.List.Signature() != before);
			checked++;
		}
	}

	// A guard on the guard: if the walk above ever found nothing to perturb,
	// every assertion in it would pass vacuously.
	CHECK(checked > 100);
}

TEST_CASE("the collector's own properties move the signature", "[gui][compile]") {
	World world("gui_compile.collector");
	const Entity screen = world.Make("ScreenGui");
	world.Make("Frame", screen);

	REQUIRE(world.Rebuild());

	for (const PropertyDescriptor &property : Classes::Describe(GuiClass("ScreenGui")).Properties) {
		if (property.Name == Name("Name") || property.Name == Name("Parent")) {
			continue;
		}

		const uint64_t before = world.List.Signature();
		if (!Perturb(world.Data, screen, property)) {
			continue;
		}

		INFO("ScreenGui." << property.Name.Text());
		world.Rebuild();
		CHECK(world.List.Signature() != before);
	}
}

TEST_CASE("the tree, the name and the screen all move the signature", "[gui][compile]") {
	World world("gui_compile.tree");
	const Entity screen = world.Make("ScreenGui");
	const Entity first = world.Make("Frame", screen);
	const Entity second = world.Make("Frame", screen);

	REQUIRE(world.Rebuild());

	// Reparenting. `ecs::Hierarchy` is not an observed component, so
	// `Store::ChangeVersion` does not move for this - which is the entire
	// reason a signature exists rather than a version compare.
	uint64_t before = world.List.Signature();
	world.Data.SetParent(second, first);
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Renaming, which only `SortOrder::Name` reads and which is folded from its
	// own pass restricted to rows that have an `Element`.
	before = world.List.Signature();
	world.Data.SetInstanceName(first, "renamed");
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Resizing the screen, which every `UDim2` resolves against.
	before = world.List.Signature();
	world.Request.Display.Width = 1024.0f;
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Destroying one.
	before = world.List.Signature();
	world.Data.DestroyInstance(second);
	world.Rebuild();
	CHECK(world.List.Signature() != before);
}

TEST_CASE("the hovered element is an input to the compile", "[gui][compile]") {
	World world("gui_compile.hover");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	// **The default fill, deliberately.** `Background::Color` is white, so a
	// button created in the explorer is white - and a shift that only ever
	// lightened clamped straight back to white and did nothing under the
	// pointer, which is every button anybody makes. This case used to be run
	// against a mid-grey, which is exactly the fill that hid it.
	REQUIRE(world.Rebuild());
	const Color3 plain = world.List.Commands().Commands.front().Tint;
	REQUIRE(plain.R == Approx(1.0f));

	world.Request.Hovered = button;
	CHECK(world.Rebuild());

	// **Shifted in the command and not in the component.** A hover written
	// back into `BackgroundColor3` would make the property read differently
	// depending on where the mouse is.
	const Color3 lit = world.List.Commands().Commands.front().Tint;
	CHECK(lit.R < plain.R);
	CHECK(world.Data.Get<Background>(button)->Color.R == Approx(plain.R));

	// And a press goes further the same way, so holding a button reads as more
	// of what hovering it started rather than as a reversal.
	world.Request.Hovered = engine::ecs::NULL_ENTITY;
	world.Request.Pressed = button;
	CHECK(world.Rebuild());
	CHECK(world.List.Commands().Commands.front().Tint.R < lit.R);
}

TEST_CASE("a dark button shifts as far as a light one", "[gui][compile]") {
	World world("gui_compile.hover.dark");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	// The same hole at the other end: darkening black is no shift either, so
	// the direction is chosen per fill rather than fixed once for all of them.
	Background background;
	background.Color = Color3{0.0f, 0.0f, 0.0f};
	world.Data.Set(button, background);

	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Commands().Commands.front().Tint.R == Approx(0.0f));

	world.Request.Hovered = button;
	CHECK(world.Rebuild());

	const Color3 lit = world.List.Commands().Commands.front().Tint;
	CHECK(lit.R > 0.0f);

	world.Request.Hovered = engine::ecs::NULL_ENTITY;
	world.Request.Pressed = button;
	CHECK(world.Rebuild());
	CHECK(world.List.Commands().Commands.front().Tint.R > lit.R);
}

TEST_CASE("paint order is parent first, then siblings by ZIndex", "[gui][compile]") {
	World world("gui_compile.order");
	const Entity screen = world.Make("ScreenGui");
	const Entity back = world.Make("Frame", screen);
	const Entity front = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", back);

	Element low;
	low.ZIndex = 1;
	world.Data.Set(back, low);

	Element high;
	high.ZIndex = 10;
	world.Data.Set(front, high);

	Element beneath;

	// **Deliberately lower than its own parent's sibling.** Under
	// `ZIndexBehavior::Sibling` a child is drawn over its parent whatever its
	// number, and `ZIndex` only orders siblings - so this child sits above
	// `back` and below `front`, and a compile that sorted globally would put it
	// at the bottom.
	beneath.ZIndex = -50;
	world.Data.Set(child, beneath);

	REQUIRE(world.Rebuild());

	const auto position = [&](Entity instance) {
		const std::vector<DrawCommand> &commands = world.List.Commands().Commands;
		for (size_t index = 0; index < commands.size(); index++) {
			if (commands[index].Source == instance) {
				return index;
			}
		}
		FAIL("no command for the instance");
		return size_t{0};
	};

	CHECK(position(back) < position(child));
	CHECK(position(child) < position(front));
}

TEST_CASE("Global ZIndex behaviour re-sorts the whole collector", "[gui][compile]") {
	World world("gui_compile.global");
	const Entity screen = world.Make("ScreenGui");
	const Entity back = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", back);
	const Entity front = world.Make("Frame", screen);

	Layer layer;
	layer.Behavior = ZIndexBehavior::Global;
	world.Data.Set(screen, layer);

	Element low;
	low.ZIndex = -50;
	world.Data.Set(child, low);

	Element mid;
	mid.ZIndex = 1;
	world.Data.Set(back, mid);

	Element high;
	high.ZIndex = 10;
	world.Data.Set(front, high);

	REQUIRE(world.Rebuild());

	const std::vector<DrawCommand> &commands = world.List.Commands().Commands;
	REQUIRE(commands.size() >= 3);

	// The legacy rule: depth does not beat the number, so the child goes to the
	// bottom.
	CHECK(commands.front().Source == child);
}

TEST_CASE("a fully transparent element emits nothing", "[gui][compile]") {
	World world("gui_compile.transparent");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	REQUIRE(world.Rebuild());
	const size_t opaque = world.List.Commands().Commands.size();
	REQUIRE(opaque > 0);

	Background background;
	background.Transparency = 1.0f;
	world.Data.Set(frame, background);

	REQUIRE(world.Rebuild());

	// Not emitted at all, so a backend never has to test for it - and the
	// command count means "what is on screen" rather than "what exists".
	CHECK(world.List.Commands().Commands.empty());

	// The element is still counted as reached, which is what tells a panel the
	// difference between an invisible element and a missing one.
	CHECK(world.List.Commands().Elements == 1);
}

TEST_CASE("an element clipped to nothing emits nothing", "[gui][compile]") {
	World world("gui_compile.clipped");
	const Entity screen = world.Make("ScreenGui");
	const Entity window = world.Make("Frame", screen);
	const Entity inside = world.Make("Frame", window);

	Element outer;
	outer.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	outer.ClipsDescendants = true;
	world.Data.Set(window, outer);

	Element pushed;
	pushed.Position = UDim2{0.0f, 5000.0f, 0.0f, 5000.0f};
	pushed.Size = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	world.Data.Set(inside, pushed);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		CHECK(command.Source != inside);
	}
}

TEST_CASE("a text box shows its placeholder when empty", "[gui][compile]") {
	World world("gui_compile.placeholder");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("TextBox", screen);

	Entry entry;
	entry.PlaceholderText = "type here";
	world.Data.Set(box, entry);

	REQUIRE(world.Rebuild());

	bool found = false;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Kind == DrawKind::Text) {
			CHECK(command.Text == "type here");
			found = true;
		}
	}
	CHECK(found);

	// One command either way, so a backend drawing "the text" does not have to
	// know which of the two strings it is.
	Label label;
	label.Text = "typed";
	world.Data.Set(box, label);

	REQUIRE(world.Rebuild());
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Kind == DrawKind::Text) {
			CHECK(command.Text == "typed");
		}
	}
}

TEST_CASE("Invalidate forces one rebuild and no more", "[gui][compile]") {
	World world("gui_compile.invalidate");
	world.Make("ScreenGui");

	REQUIRE(world.Rebuild());
	REQUIRE_FALSE(world.Rebuild());

	// A backend that lost its device state wants the list resubmitted although
	// the list is correct.
	world.List.Invalidate();
	CHECK(world.Rebuild());
	CHECK_FALSE(world.Rebuild());
}

TEST_CASE("a canvas group multiplies colour and opacity through its subtree", "[gui][compile]") {
	World world("gui_compile.group");
	const Entity screen = world.Make("ScreenGui");
	const Entity groupEntity = world.Make("CanvasGroup", screen);
	const Entity child = world.Make("Frame", groupEntity);

	Group group;
	group.Color = Color3{0.5f, 0.25f, 1.0f};
	group.Transparency = 0.5f;
	world.Data.Set(groupEntity, group);

	Background background;
	background.Color = Color3{0.8f, 0.4f, 0.2f};
	background.Transparency = 0.25f;
	world.Data.Set(child, background);

	REQUIRE(world.Rebuild());
	const auto found = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) {
			return command.Source == child && command.Kind == DrawKind::Rectangle;
		}
	);
	REQUIRE(found != world.List.Commands().Commands.end());
	CHECK(found->Tint.R == Approx(0.4f));
	CHECK(found->Tint.G == Approx(0.1f));
	CHECK(found->Tint.B == Approx(0.2f));
	CHECK(found->Transparency == Approx(0.625f));
}

TEST_CASE("a viewport frame emits its camera image with authored tint", "[gui][compile]") {
	World world("gui_compile.viewport");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ViewportFrame", screen);

	Viewport viewport;
	viewport.CurrentCamera = Entity{42};
	viewport.Color = Color3{0.5f, 0.75f, 0.25f};
	viewport.Transparency = 0.2f;
	world.Data.Set(frame, viewport);

	REQUIRE(world.Rebuild());
	const auto found = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) {
			return command.Source == frame && command.Kind == DrawKind::Viewport;
		}
	);
	REQUIRE(found != world.List.Commands().Commands.end());
	CHECK(found->Tint == viewport.Color);
	CHECK(found->Transparency == Approx(0.2f));
}

TEST_CASE("a scrolling frame emits proportional bars over its children", "[gui][compile]") {
	World world("gui_compile.scrollbars");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ScrollingFrame", screen);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	Scrolling scrolling;
	scrolling.CanvasSize = UDim2{2.0f, 0.0f, 3.0f, 0.0f};
	scrolling.CanvasPosition = Vector2{50.0f, 100.0f};
	scrolling.Direction = ScrollingDirection::XY;
	scrolling.BarThickness = 10;
	world.Data.Set(frame, scrolling);

	REQUIRE(world.Rebuild());
	std::vector<const DrawCommand *> rectangles;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Rectangle) {
			rectangles.push_back(&command);
		}
	}

	REQUIRE(rectangles.size() == 3);
	const DrawCommand &vertical = *rectangles[1];
	const DrawCommand &horizontal = *rectangles[2];
	CHECK(vertical.Bounds.Width() == Approx(10.0f));
	CHECK(horizontal.Bounds.Height() == Approx(10.0f));
	CHECK(vertical.CornerRadius == Approx(5.0f));
	CHECK(horizontal.CornerRadius == Approx(5.0f));

	// **The full height, because neither inset is reserved.** With
	// `ScrollBarInset::None` - Roblox's default and this one - the two bars
	// overlap in the corner rather than shortening each other's track, so a
	// hundred-pixel frame showing a third of a three-hundred-pixel canvas gets a
	// third of a hundred-pixel track.
	CHECK(vertical.Bounds.Height() == Approx(100.0f / 3.0f));
	CHECK(horizontal.Bounds.Width() == Approx(50.0f));

	// And reserving both strips shortens the window, which shortens both the
	// track and the share of it each thumb takes.
	scrolling.HorizontalInset = ScrollBarInset::Always;
	scrolling.VerticalInset = ScrollBarInset::Always;
	world.Data.Set(frame, scrolling);
	REQUIRE(world.Rebuild());

	const ScrollState *state = world.Data.Get<ScrollState>(frame);
	REQUIRE(state != nullptr);
	CHECK(state->WindowSize.X == Approx(90.0f));
	CHECK(state->WindowSize.Y == Approx(90.0f));
	CHECK(state->CanvasSize.X == Approx(180.0f));
	CHECK(state->VerticalThumb.Height() == Approx(90.0f * 90.0f / 270.0f));
}

TEST_CASE("an automatic canvas grows to what the frame holds", "[gui][compile]") {
	// The property has been declared and unread since the tree went in; this is
	// the case that makes it mean something. A list of three fixed rows in a
	// frame shorter than they are is exactly what it is for.
	World world("gui_compile.autocanvas");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ScrollingFrame", screen);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	Scrolling scrolling;
	scrolling.CanvasSize = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	scrolling.AutomaticCanvas = AutomaticSize::Y;
	world.Data.Set(frame, scrolling);

	const Entity layout = world.Make("UIListLayout", frame);
	world.Data.Set(layout, ListLayout{});

	for (int index = 0; index < 3; index++) {
		const Entity row = world.Make("Frame", frame);
		Element size;
		size.Size = UDim2{0.0f, 100.0f, 0.0f, 60.0f};
		size.LayoutOrder = index;
		world.Data.Set(row, size);
	}

	REQUIRE(world.Rebuild());

	const ScrollState *state = world.Data.Get<ScrollState>(frame);
	REQUIRE(state != nullptr);

	// **The content replaces that axis rather than adding to it**, which is
	// Roblox's rule: three sixty-pixel rows are a hundred and eighty pixels of
	// canvas whatever `CanvasSize` said.
	CHECK(state->CanvasSize.Y == Approx(180.0f));

	// The other axis is untouched, so a frame that grows downwards does not
	// start scrolling sideways.
	CHECK(state->CanvasSize.X == Approx(0.0f));
}

TEST_CASE("a billboard is hidden from the one viewer it names", "[gui][compile]") {
	// **A compile decision and not a layout one**, which is what the two halves
	// of this case check: the tree resolves identically for everybody - the
	// billboard is `Rendered` either way - and what differs is only which
	// commands reach the list. Deciding it in the backend instead would leave a
	// compiled list that disagreed with the frame drawn from it.
	World world("gui_compile.hidefrom");

	const Entity workspace =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Workspace");

	const Entity viewer =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Somebody");
	const Entity other =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "SomebodyElse");

	const Entity billboard = world.Make("BillboardGui", workspace);
	const Entity frame = world.Make("Frame", billboard);
	world.Data.Set(frame, Element{});

	const auto drawn = [&] {
		size_t found = 0;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			found += command.Source == frame ? 1 : 0;
		}
		return found;
	};

	Billboard state;
	state.PlayerToHideFrom = viewer;
	world.Data.Set(billboard, state);

	world.Request.Viewer = other;
	REQUIRE(world.Rebuild());
	CHECK(drawn() > 0);

	world.Request.Viewer = viewer;
	REQUIRE(world.Rebuild());
	CHECK(drawn() == 0);

	// **The tree is untouched either way.** A viewer-specific decision that had
	// reached `Resolved` would make one client's `AbsoluteSize` differ from
	// another's for an element neither of them authored differently.
	CHECK(world.Data.Get<Resolved>(frame)->Rendered);

	// And nobody in particular hides nothing, which is the studio's case and a
	// test's - an unset `PlayerToHideFrom` is also null and must not match.
	world.Request.Viewer = engine::ecs::NULL_ENTITY;
	REQUIRE(world.Rebuild());
	CHECK(drawn() > 0);
}

TEST_CASE("a gradient resolves into the line it ramps along", "[gui][compile]") {
	// **The compile's whole job for a `UIGradient`**: an angle and a size-
	// relative offset go in, two points in canvas pixels come out. Doing it here
	// rather than in a backend is what stops the client and the studio
	// disagreeing about where a ramp starts.
	World world("gui_compile.gradient");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.0f, 100.0f, 0.0f, 50.0f};
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	const Entity ramp = world.Make("UIGradient", frame);
	world.Data.Set(ramp, Gradient{});

	REQUIRE(world.Rebuild());

	const DrawCommand *fill = nullptr;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Rectangle) {
			fill = &command;
			break;
		}
	}

	REQUIRE(fill != nullptr);
	REQUIRE(fill->Gradient >= 0);
	const DrawGradient &resolved = world.List.Commands().Gradients[std::size_t(fill->Gradient)];

	// Unrotated, so the ramp runs left to right across the element's own width.
	CHECK(resolved.Origin.X == Approx(100.0f));
	CHECK(resolved.Origin.Y == Approx(100.0f));
	CHECK(resolved.Axis.X == Approx(200.0f));
	CHECK(resolved.Axis.Y == Approx(0.0f).margin(0.001f));

	// **A quarter turn ramps over the full height, not over the width.** The
	// ends snap to the element's edges *along the rotated axis*, which is what
	// makes a ninety-degree gradient a top-to-bottom one rather than a diagonal.
	Gradient turned;
	turned.Rotation = 90.0f;
	world.Data.Set(ramp, turned);
	REQUIRE(world.Rebuild());

	const DrawGradient &vertical = world.List.Commands().Gradients.front();
	CHECK(vertical.Axis.X == Approx(0.0f).margin(0.001f));
	CHECK(vertical.Axis.Y == Approx(100.0f));
	CHECK(vertical.Origin.X == Approx(200.0f));
	CHECK(vertical.Origin.Y == Approx(50.0f));

	// The offset is in multiples of the element's own size, so one whole width
	// slides the ramp entirely off the right-hand edge.
	Gradient slid;
	slid.Offset = Vector2{1.0f, 0.0f};
	world.Data.Set(ramp, slid);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Gradients.front().Origin.X == Approx(300.0f));

	// And a disabled one resolves to nothing at all rather than to a ramp that
	// happens to be flat - a backend that got the second would still subdivide.
	Gradient off;
	off.Enabled = false;
	world.Data.Set(ramp, off);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Gradients.empty());
	for (const DrawCommand &command : world.List.Commands().Commands) {
		CHECK(command.Gradient == -1);
	}
}

TEST_CASE("a stroke takes its own gradient and never the element's", "[gui][compile]") {
	// Roblox's arrangement, and the reason it is worth a case: an outline is a
	// separate thing with a separate colour, so inheriting the fill's ramp would
	// tint it with one nobody asked for.
	World world("gui_compile.strokegradient");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Data.Set(frame, Element{});

	const Entity fillRamp = world.Make("UIGradient", frame);
	Gradient warm;
	warm.Color = engine::core::ColorSequence{Color3{1.0f, 0.0f, 0.0f}};
	world.Data.Set(fillRamp, warm);

	const Entity stroke = world.Make("UIStroke", frame);
	world.Data.Set(stroke, Stroke{});

	REQUIRE(world.Rebuild());

	const DrawCommand *outline = nullptr;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Outline) {
			outline = &command;
		}
	}

	// The border and the stroke are both outlines; the stroke is the last one
	// this element emits, which is what `Emit` promises.
	REQUIRE(outline != nullptr);
	CHECK(outline->Gradient == -1);

	const Entity strokeRamp = world.Make("UIGradient", stroke);
	Gradient cool;
	cool.Color = engine::core::ColorSequence{Color3{0.0f, 0.0f, 1.0f}};
	world.Data.Set(strokeRamp, cool);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Outline) {
			outline = &command;
		}
	}

	REQUIRE(outline->Gradient >= 0);
	const DrawGradient &ramp = world.List.Commands().Gradients[std::size_t(outline->Gradient)];
	CHECK(ramp.Color.Evaluate(0.5f).B == Approx(1.0f));
}

TEST_CASE("a rich text label reaches the list as spans over one string", "[gui][compile]") {
	// **One command and not several**, which is the arrangement the whole
	// feature turns on: only a backend can measure a glyph, so a marked-up run
	// has to arrive as one string it lays out in one pass.
	World world("gui_compile.richtext");

	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	world.Data.Set(label, Element{});

	Label text;
	text.Text = "a <b>big</b> word";
	text.Rich = true;
	world.Data.Set(label, text);

	REQUIRE(world.Rebuild());

	const DrawCommand *run = nullptr;
	size_t runs = 0;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == label && command.Kind == DrawKind::Text) {
			run = &command;
			runs++;
		}
	}

	REQUIRE(run != nullptr);
	CHECK(runs == 1);
	CHECK(run->Text == "a big word");
	REQUIRE(run->Spans.size() == 1);
	CHECK(run->Spans[0].Font == FontFace::Bold);

	// Without the property the markup is text, which is what an author who
	// wrote a less-than sign into a plain label meant.
	text.Rich = false;
	world.Data.Set(label, text);
	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == label && command.Kind == DrawKind::Text) {
			CHECK(command.Text == "a <b>big</b> word");
			CHECK(command.Spans.empty());
		}
	}
}

TEST_CASE("the visible limit cuts the text and the spans together", "[gui][compile]") {
	// Roblox applies `MaxVisibleGraphemes` to what a reader sees, so a
	// typewriter effect on a marked-up string reveals letters and not tags - and
	// a span left pointing past the cut would be a range a backend indexes with.
	World world("gui_compile.maxvisible");

	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	world.Data.Set(label, Element{});

	Label text;
	text.Text = "<b>abcdef</b>";
	text.Rich = true;
	text.MaxVisible = 3;
	world.Data.Set(label, text);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source != label || command.Kind != DrawKind::Text) {
			continue;
		}
		CHECK(command.Text == "abc");
		REQUIRE(command.Spans.size() == 1);
		CHECK(command.Spans[0].End == 3);
	}

	// And the measurement follows, so `TextScaled` fits what is shown rather
	// than what was typed.
	const Resolved *resolved = world.Data.Get<Resolved>(label);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->TextBounds.X == Approx(3.0f * AVERAGE_ADVANCE * float(resolved->TextSize)));
}

TEST_CASE("a scaled stroke resolves to pixels before it reaches a backend", "[gui][compile]") {
	// **A draw list has no element to measure against**, so a fraction that
	// survived the compile would be a thickness a backend could not resolve -
	// and the one that arrived at `InterfaceMesh` would be read as pixels and
	// draw a hairline. See `StrokeThickness`.
	World world("gui_compile.strokesizing");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	// 200 wide and 80 tall, so the smaller side is 80 and the two modes cannot
	// be confused by a square.
	Element box;
	box.Size = UDim2{0.0f, 200.0f, 0.0f, 80.0f};
	world.Data.Set(frame, box);

	const Entity stroke = world.Make("UIStroke", frame);

	const auto thicknessOf = [&]() {
		REQUIRE(world.Rebuild());
		const DrawCommand *outline = nullptr;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			if (command.Source == frame && command.Kind == DrawKind::Outline) {
				outline = &command;
			}
		}
		REQUIRE(outline != nullptr);
		return outline->Thickness;
	};

	SECTION("fixed is the number that was typed") {
		Stroke pixels;
		pixels.Thickness = 3.0f;
		pixels.Sizing = StrokeSizing::FixedSize;
		world.Data.Set(stroke, pixels);
		CHECK(thicknessOf() == Approx(3.0f));
	}

	SECTION("scaled is a fraction of the smaller side") {
		Stroke fraction;
		fraction.Thickness = 0.1f;
		fraction.Sizing = StrokeSizing::ScaledSize;
		world.Data.Set(stroke, fraction);

		// The smaller side, not the larger and not the average: an outline
		// scaled by a long thin element's width would be thicker than the
		// element is tall.
		CHECK(thicknessOf() == Approx(8.0f));
	}

	SECTION("a scaled zero draws nothing, like a fixed zero") {
		// **Counted rather than asserted absent**, because a `Frame` carries a
		// one-pixel `Background::BorderSizePixel` and that is an outline too.
		// Counting is the stronger question anyway: it says the stroke is a
		// second command rather than something that edits the border.
		const auto outlines = [&]() {
			REQUIRE(world.Rebuild());
			size_t found = 0;
			for (const DrawCommand &command : world.List.Commands().Commands) {
				found += command.Source == frame && command.Kind == DrawKind::Outline ? 1 : 0;
			}
			return found;
		};

		Stroke drawn;
		drawn.Thickness = 0.1f;
		drawn.Sizing = StrokeSizing::ScaledSize;
		world.Data.Set(stroke, drawn);
		const size_t withStroke = outlines();

		Stroke none = drawn;
		none.Thickness = 0.0f;
		world.Data.Set(stroke, none);
		CHECK(outlines() == withStroke - 1);
	}
}

TEST_CASE("a stroke's join and sizing reach the draw list and the signature", "[gui][compile]") {
	// The signature is what decides whether a frame recompiles at all, so a
	// property that changed the picture without changing the hash would draw
	// the old one until something else moved.
	World world("gui_compile.strokejoin");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Data.Set(frame, Element{});
	const Entity stroke = world.Make("UIStroke", frame);

	Stroke round;
	round.Thickness = 2.0f;
	round.Join = LineJoin::Round;
	world.Data.Set(stroke, round);
	REQUIRE(world.Rebuild());

	const auto joinOf = [&]() {
		const DrawCommand *outline = nullptr;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			if (command.Source == frame && command.Kind == DrawKind::Outline) {
				outline = &command;
			}
		}
		REQUIRE(outline != nullptr);
		return outline->Join;
	};
	CHECK(joinOf() == LineJoin::Round);

	Stroke mitred = round;
	mitred.Join = LineJoin::Miter;
	world.Data.Set(stroke, mitred);
	REQUIRE(world.Rebuild());
	CHECK(joinOf() == LineJoin::Miter);

	// And the sizing half of the same argument: switching mode changes the
	// thickness, so it has to move the hash even though the typed number did
	// not change.
	Stroke scaled = mitred;
	scaled.Sizing = StrokeSizing::ScaledSize;
	world.Data.Set(stroke, scaled);
	REQUIRE(world.Rebuild());
}
