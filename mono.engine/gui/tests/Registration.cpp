#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.registration")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::ecs::TypeDescriptor;

using namespace engine::gui;

namespace {
	// Every component name this module promises. A file, a wire and a recording
	// all carry these strings, so renaming one is a format change and this list
	// is what makes it show up as a failing test rather than as a save that
	// loads into a narrower world.
	const std::vector<std::string_view> EXPECTED_COMPONENTS{
		"gui.Element",	  "gui.Background",	 "gui.Label",	   "gui.Picture",		 "gui.Button",
		"gui.Scrolling",  "gui.Entry",		 "gui.Layer",	   "gui.Canvas",		 "gui.Surface",
		"gui.Billboard",  "gui.Group",		 "gui.Viewport",   "gui.Padding",		 "gui.ListLayout",
		"gui.GridLayout", "gui.AspectRatio", "gui.SizeLimits", "gui.TextSizeLimits", "gui.Corner",
		"gui.Stroke",	  "gui.Scale",		 "gui.Resolved",
	};
}

TEST_CASE("every component registers under its promised name", "[gui][registration]") {
	RegisterGuiComponents();

	for (const std::string_view name : EXPECTED_COMPONENTS) {
		INFO("component: " << name);
		const engine::ecs::ComponentId id = Components::Find(Name(name));
		REQUIRE(id.IsValid());

		// Explicit rather than the compiler's spelling, which differs between
		// compilers and would put `engine::gui::Element` as GCC happens to
		// print it into a save file.
		CHECK(Components::Describe(id).Name.Text() == name);
	}
}

TEST_CASE("the class tree registers every promised class", "[gui][registration]") {
	RegisterGuiClasses();

	for (const std::string_view name : GuiClassNames()) {
		INFO("class: " << name);
		CHECK(GuiClass(name).IsValid());
	}

	// The list is a contract in both directions: a class registered and not
	// listed would go unmentioned by the palette and the manifest.
	//
	// **Forty-five**: the thirty-three of the 2D tree, `GuiService`, and the
	// eleven of the 3D branch. The service is in this list rather than in
	// `scene`'s because it is a `gui` class — the two modules may not link each
	// other — and it is registered at all because it owns the selection, which
	// is what finally gave `GuiObject::Selectable` a reader.
	CHECK(GuiClassNames().size() == 45);
}

TEST_CASE("the 2D tree descends the way a script expects", "[gui][registration]") {
	RegisterGuiClasses();

	// `:IsA` is set inclusion over the class tree, so these are the relations a
	// migrating script already relies on. Breaking one would not fail to
	// compile — a query for `GuiObject` would simply stop matching.
	CHECK(Classes::IsA(GuiClass("TextButton"), GuiClass("GuiButton")));
	CHECK(Classes::IsA(GuiClass("TextButton"), GuiClass("GuiObject")));
	CHECK(Classes::IsA(GuiClass("TextButton"), GuiClass("GuiBase2d")));
	CHECK(Classes::IsA(GuiClass("ImageLabel"), GuiClass("GuiLabel")));
	CHECK(Classes::IsA(GuiClass("ScrollingFrame"), GuiClass("Frame")));
	CHECK(Classes::IsA(GuiClass("CanvasGroup"), GuiClass("Frame")));
	CHECK(Classes::IsA(GuiClass("ScreenGui"), GuiClass("LayerCollector")));
	CHECK(Classes::IsA(GuiClass("DockWidgetPluginGui"), GuiClass("PluginGui")));
	CHECK(Classes::IsA(GuiClass("UIListLayout"), GuiClass("UILayout")));
	CHECK(Classes::IsA(GuiClass("UIAspectRatioConstraint"), GuiClass("UIConstraint")));

	// **A collector is not a `GuiObject`**, which is the one relation people
	// assume and Roblox does not have. A `ScreenGui` has no `Position` and no
	// `BackgroundColor3`; if it derived from `GuiObject` it would have both and
	// the layout would try to resolve them against a parent it does not have.
	CHECK_FALSE(Classes::IsA(GuiClass("ScreenGui"), GuiClass("GuiObject")));

	// And a modifier is not a `GuiBase`. A `UIPadding` under a frame is a child
	// in the tree and must never be laid out as one.
	CHECK_FALSE(Classes::IsA(GuiClass("UIPadding"), GuiClass("GuiBase")));
}

TEST_CASE("a fully populated Label round-trips through its serialiser", "[gui][registration]") {
	RegisterGuiComponents();

	// **Every field set to something other than its default.** A round trip of
	// a default-valued component passes whatever the serialiser forgot, which
	// is exactly the bug `scene::WriteVisuals` records having shipped twice.
	Label written;
	written.Text = "Hello, cave";
	written.Color = engine::core::Color3{0.1f, 0.2f, 0.3f};
	written.Transparency = 0.25f;
	written.Size = 37;
	written.Font = FontFace::Code;
	written.XAlignment = TextXAlignment::Right;
	written.YAlignment = TextYAlignment::Bottom;
	written.Wrapped = true;
	written.Scaled = true;
	written.Truncate = TextTruncate::AtEnd;
	written.StrokeColor = engine::core::Color3{0.4f, 0.5f, 0.6f};
	written.StrokeTransparency = 0.75f;
	written.LineHeight = 1.5f;

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<Label>());

	ByteWriter writer;
	descriptor.Write(writer, &written, 1);

	Label read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	CHECK(read.Text == written.Text);
	CHECK(read.Color.R == written.Color.R);
	CHECK(read.Color.G == written.Color.G);
	CHECK(read.Color.B == written.Color.B);
	CHECK(read.Transparency == written.Transparency);
	CHECK(read.Size == written.Size);
	CHECK(read.Font == written.Font);
	CHECK(read.XAlignment == written.XAlignment);
	CHECK(read.YAlignment == written.YAlignment);
	CHECK(read.Wrapped == written.Wrapped);
	CHECK(read.Scaled == written.Scaled);
	CHECK(read.Truncate == written.Truncate);
	CHECK(read.StrokeColor.R == written.StrokeColor.R);
	CHECK(read.StrokeTransparency == written.StrokeTransparency);
	CHECK(read.LineHeight == written.LineHeight);
}

TEST_CASE("a fully populated Picture round-trips through its serialiser", "[gui][registration]") {
	RegisterGuiComponents();

	Picture written;
	written.Image = Name("rbxasset://textures/panel.png");
	written.Color = engine::core::Color3{0.9f, 0.8f, 0.7f};
	written.Transparency = 0.5f;
	written.Scale = ScaleType::Slice;
	written.SliceCenter = engine::core::Rect{4.0f, 5.0f, 60.0f, 61.0f};
	written.SliceScale = 2.0f;
	written.TileSize = engine::core::UDim2{0.25f, 3.0f, 0.5f, 7.0f};
	written.RectOffset = engine::core::Vector2{11.0f, 12.0f};
	written.RectSize = engine::core::Vector2{13.0f, 14.0f};

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<Picture>());

	ByteWriter writer;
	descriptor.Write(writer, &written, 1);

	Picture read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	CHECK(read.Image == written.Image);
	CHECK(read.Transparency == written.Transparency);
	CHECK(read.Scale == written.Scale);
	CHECK(read.SliceCenter == written.SliceCenter);
	CHECK(read.SliceScale == written.SliceScale);
	CHECK(read.TileSize == written.TileSize);
	CHECK(read.RectOffset == written.RectOffset);
	CHECK(read.RectSize == written.RectSize);
}

TEST_CASE("a text box's caret does not cross a save", "[gui][registration]") {
	RegisterGuiComponents();

	// Where somebody's cursor is is not a fact about the game. A save that
	// restored a text box mid-edit would be restoring a session, and a replica
	// that received one would move the local player's caret.
	Entry written;
	written.PlaceholderText = "type here";
	written.CursorPosition = 4;
	written.SelectionStart = 2;

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<Entry>());

	ByteWriter writer;
	descriptor.Write(writer, &written, 1);

	Entry read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	CHECK(read.PlaceholderText == written.PlaceholderText);
	CHECK(read.CursorPosition == -1);
	CHECK(read.SelectionStart == -1);
}

TEST_CASE("writing text every frame interns nothing", "[gui][registration]") {
	// **`D00020` closed, and this is the case that says so in the only terms
	// that matter.** `Label::Text` was a `core::Name`, and `core::Name` never
	// releases: `label.Text = tostring(score)` at sixty hertz grew the
	// process-wide registry forever and took its mutex inside the frame loop to
	// do it. A score counter is the first thing anybody writes, so the leak was
	// not exotic.
	//
	// Counted rather than reasoned about. A thousand distinct strings through
	// the property surface — the same path a script takes — and the registry
	// must not have moved at all.
	RegisterGuiClasses();

	Store store("gui_registration.owned_text");
	const Entity label = store.CreateInstance(GuiClass("TextLabel"), "Score");

	// One write first, so any interning the *path* does — the property name,
	// the class name, a lazily built table — has already happened and is not
	// counted against the text.
	const std::string first = "warm";
	REQUIRE(store.SetProperty(label, Name("Text"), &first, sizeof(first)));

	const size_t before = Name::Count();

	for (int frame = 0; frame < 1000; frame++) {
		const std::string value = "Score: " + std::to_string(frame);
		REQUIRE(store.SetProperty(label, Name("Text"), &value, sizeof(value)));
	}

	CHECK(Name::Count() == before);

	// And the last one is what the component holds, so the writes were real
	// rather than being refused a thousand times in a row.
	const Label *state = store.Get<Label>(label);
	REQUIRE(state != nullptr);
	CHECK(state->Text == "Score: 999");
}

TEST_CASE("an image name still interns, and should", "[gui][registration]") {
	// **The other half of the split, pinned so it cannot drift.** An asset id
	// is one of the bounded set of things a game shipped, so interning it is
	// what makes it an integer comparison everywhere downstream — the same trade
	// `Material` and every class name make. A change that converted every string
	// in this module to owned storage would pass the case above and quietly cost
	// this.
	RegisterGuiClasses();

	Store store("gui_registration.interned_image");
	const Entity picture = store.CreateInstance(GuiClass("ImageLabel"), "Wall");

	const Name warm("rbxasset://textures/warm");
	REQUIRE(store.SetProperty(picture, Name("Image"), &warm, sizeof(warm)));

	const size_t before = Name::Count();
	const Name fresh("rbxasset://textures/a-name-no-other-case-here-uses");
	REQUIRE(store.SetProperty(picture, Name("Image"), &fresh, sizeof(fresh)));

	CHECK(Name::Count() > before);
}
