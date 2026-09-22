#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Invariants.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Style.hpp>
#include <engine/gui/VirtualCollection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.gui.registration")
TEST_DEPENDS("engine.ecs.invariants")

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
		"gui.Element",
		"gui.Background",
		"gui.Label",
		"gui.LabelPresentation",
		"gui.LabelLocalizationArguments",
		"gui.Picture",
		"gui.Button",
		"gui.Scrolling",
		"gui.Entry",
		"gui.Layer",
		"gui.Canvas",
		"gui.CanvasTransform",
		"gui.Surface",
		"gui.Billboard",
		"gui.Group",
		"gui.Viewport",
		"gui.Padding",
		"gui.ListLayout",
		"gui.GridLayout",
		"gui.AspectRatio",
		"gui.SizeLimits",
		"gui.TextSizeLimits",
		"gui.Corner",
		"gui.Stroke",
		"gui.Scale",
		"gui.Resolved",
		"gui.SpatialCanvas",
		"gui.GuiServiceState",
		"gui.TextCompositionState",
		"gui.Adornment",
		"gui.AdornmentInteraction",
		"gui.SelectionOutline",
		"gui.HandleShape",
		"gui.FlexItem",

		// v0.18: the gradient, the scrolling frame's derived state, the
		// selection handles, and the two layouts that arrived with them.
		"gui.Gradient",
		"gui.ScrollState",
		"gui.Selection",
		"gui.TableLayout",
		"gui.PageLayout",
		"gui.DragDetector",
		"gui.PageMotion",
		"gui.ScrollMotion",
		"gui.SettingsMenuExtensions",
		"gui.BoxHandleShape",
		"gui.SphereHandleShape",
		"gui.CylinderHandleShape",
		"gui.LineHandleShape",
		"gui.ConeHandleShape",
		"gui.HandlesShape",
		"gui.ArcHandlesShape",
		"gui.NodeCanvas",
		"gui.NodeCanvasNode",
		"gui.NodeCanvasGroup",
		"gui.NodeCanvasPort",
		"gui.NodeCanvasLink",
		"gui.Binding",
		"gui.BindingOutput",
		"gui.BindingDependency",
		"gui.VirtualCollection",
		"gui.VirtualFocusState",
		"gui.VirtualAnchorState",
		"gui.ModalScope",
		"gui.AnimationPlayback",
		"gui.PresentationState",
		"gui.UITheme",
		"gui.StyleClass",
		"gui.StyleDirect",
		"gui.UIStyle",
		"gui.ThemeBinding",
		"gui.ResolvedStyle",
		"gui.Mask",
	};

	struct ExpectedProperty {
		std::string_view Class;
		std::string_view Property;
		bool Writable = true;
	};

	// The exact v0.20 GUI surface. The Studio Properties panel and both script
	// bindings consume this same descriptor list, so absence or wrong mutability
	// here is absence or wrong mutability in all three places.
	const std::vector<ExpectedProperty> V020_PROPERTIES{
		{"SurfaceGui", "ZOffset"},
		{"SurfaceGui", "MaxDistance"},
		{"SurfaceGui", "ClipsDescendants"},
		{"SurfaceGui", "Active"},
		{"BillboardGui", "Active"},
		{"BillboardGui", "Brightness"},
		{"BillboardGui", "ClipsDescendants"},
		{"BillboardGui", "CurrentDistance", false},
		{"BillboardGui", "DistanceStep"},
		{"BillboardGui", "ExtentsOffsetWorldSpace"},
		{"BillboardGui", "SizeOffset"},
		{"BillboardGui", "PlayerToHideFrom"},
		{"ScrollingFrame", "ScrollingEnabled"},
		{"ScrollingFrame", "AutomaticCanvasSize"},
		{"ScrollingFrame", "HorizontalScrollBarInset"},
		{"ScrollingFrame", "VerticalScrollBarInset"},
		{"ScrollingFrame", "VerticalScrollBarPosition"},
		{"ScrollingFrame", "ElasticBehavior"},
		{"ScrollingFrame", "TopImage"},
		{"ScrollingFrame", "MidImage"},
		{"ScrollingFrame", "BottomImage"},
		{"ScrollingFrame", "AbsoluteCanvasSize", false},
		{"ScrollingFrame", "AbsoluteWindowSize", false},
		{"GuiObject", "Interactable"},
		{"GuiObject", "NextSelectionUp"},
		{"GuiObject", "NextSelectionDown"},
		{"GuiObject", "NextSelectionLeft"},
		{"GuiObject", "NextSelectionRight"},
		{"GuiObject", "SelectionOrder"},
		{"GuiObject", "SelectionImageObject"},
		{"ImageButton", "HoverImage"},
		{"ImageButton", "PressedImage"},
		{"ImageButton", "ResampleMode"},
		{"ImageLabel", "ResampleMode"},
		{"UIStroke", "Enabled"},
		{"UIStroke", "ApplyStrokeMode"},
		{"UIGradient", "Color"},
		{"UIGradient", "Transparency"},
		{"UIGradient", "Offset"},
		{"UIGradient", "Rotation"},
		{"UIGradient", "Enabled"},
		{"UITableLayout", "Padding"},
		{"UITableLayout", "FillEmptySpaceColumns"},
		{"UITableLayout", "FillEmptySpaceRows"},
		{"UITableLayout", "FillDirection"},
		{"UITableLayout", "HorizontalAlignment"},
		{"UITableLayout", "VerticalAlignment"},
		{"UITableLayout", "SortOrder"},
		{"UIPageLayout", "CurrentPage"},
		{"UIPageLayout", "Padding"},
		{"UIPageLayout", "Circular"},
		{"UIPageLayout", "FillDirection"},
		{"UIPageLayout", "SortOrder"},
		{"UIPageLayout", "Animated"},
		{"UIPageLayout", "TweenTime"},
		{"UIPageLayout", "EasingStyle"},
		{"UIPageLayout", "EasingDirection"},
		{"UIDragDetector", "BoundingUI"},
		{"UIDragDetector", "DragAxis"},
		{"UIDragDetector", "MinDragTranslation"},
		{"UIDragDetector", "MaxDragTranslation"},
		{"UIDragDetector", "Enabled"},
		{"UIDragDetector", "DragStyle"},
		{"UIDragDetector", "ResponseStyle"},
		{"UIModalScope", "Enabled"},
		{"UIAnimation", "StartTime"},
		{"UIAnimation", "Playing"},
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

TEST_CASE("the complete v0.20 GUI surface reaches descriptor consumers", "[gui][registration]") {
	RegisterGuiClasses();
	Store store("gui_registration.v020_surface");

	for (const ExpectedProperty &expected : V020_PROPERTIES) {
		INFO("property: " << expected.Class << "." << expected.Property);
		const Entity instance = store.CreateInstance(GuiClass(expected.Class), std::string(expected.Class));
		REQUIRE(instance != engine::ecs::NULL_ENTITY);

		const engine::ecs::PropertyDescriptor *found = nullptr;
		for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Name == Name(expected.Property)) {
				found = &property;
				break;
			}
		}
		REQUIRE(found != nullptr);
		CHECK(found->Writable == expected.Writable);
	}

	for (const std::string_view textClass : {"TextButton", "TextLabel", "TextBox"}) {
		for (const ExpectedProperty &expected : std::vector<ExpectedProperty>{
				 {textClass, "RichText"},
				 {textClass, "MaxVisibleGraphemes"},
				 {textClass, "ContentText", false},
				 {textClass, "TextBounds", false},
				 {textClass, "TextFits", false},
			 }) {
			INFO("property: " << expected.Class << "." << expected.Property);
			const Entity instance =
				store.CreateInstance(GuiClass(expected.Class), std::string(expected.Class));
			REQUIRE(instance != engine::ecs::NULL_ENTITY);

			const engine::ecs::PropertyDescriptor *found = nullptr;
			for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Name == Name(expected.Property)) {
					found = &property;
					break;
				}
			}
			REQUIRE(found != nullptr);
			CHECK(found->Writable == expected.Writable);
		}
	}
}

TEST_CASE("the class tree registers every promised class", "[gui][registration]") {
	RegisterGuiClasses();

	for (const std::string_view name : GuiClassNames()) {
		INFO("class: " << name);
		CHECK(GuiClass(name).IsValid());
	}
	CHECK_FALSE(Classes::Describe(GuiClass("GuiService")).Creatable);

	for (const std::string_view name : {
			 "GuiBase",
			 "GuiBase2d",
			 "GuiObject",
			 "GuiButton",
			 "GuiLabel",
			 "LayerCollector",
			 "GuiBase3d",
			 "PVAdornment",
			 "HandleAdornment",
			 "UIBase",
			 "UIComponent",
			 "UILayout",
			 "UIConstraint",
			 "PluginGui",
		 }) {
		INFO(name);
		const auto &info = Classes::Describe(GuiClass(name));
		CHECK_FALSE(info.Creatable);
		CHECK_FALSE(info.StudioVisible);
		CHECK(info.Kind == engine::ecs::ClassKind::Abstract);
	}

	const auto &service = Classes::Describe(GuiClass("GuiService"));
	CHECK_FALSE(service.StudioVisible);
	CHECK(service.Kind == engine::ecs::ClassKind::Service);

	for (const std::string_view name : {
			 "Frame",
			 "TextButton",
			 "TextLabel",
			 "ScreenGui",
			 "DockWidgetPluginGui",
			 "UIListLayout",
			 "UIAspectRatioConstraint",
			 "UIMask",
			 "UIBinding",
			 "UIModalScope",
			 "UIAnimation",
			 "UIVirtualCollection",
			 "UITheme",
			 "UIStyle",
			 "BoxHandleAdornment",
		 }) {
		INFO(name);
		const auto &info = Classes::Describe(GuiClass(name));
		CHECK(info.Creatable);
		CHECK(info.StudioVisible);
		CHECK(info.Kind == engine::ecs::ClassKind::Concrete);
	}

	// The list is a contract in both directions: a class registered and not
	// listed would go unmentioned by the palette and the manifest.
	//
	// **Sixty-three**: the forty-three of the 2D tree including the five node graph
	// objects, `GuiService`, and the twelve of the 3D branch. The service is in
	// `scene`'s because it is a `gui` class - the two modules may not link each
	// other - and it is registered at all because it owns the selection, which
	// is what finally gave `GuiObject::Selectable` a reader.
	CHECK(GuiClassNames().size() == 63);
}

TEST_CASE("the 2D tree descends the way a script expects", "[gui][registration]") {
	RegisterGuiClasses();

	// `:IsA` is set inclusion over the class tree, so these are the relations a
	// migrating script already relies on. Breaking one would not fail to
	// compile - a query for `GuiObject` would simply stop matching.
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
	CHECK(Classes::IsA(GuiClass("ConeHandleAdornment"), GuiClass("HandleAdornment")));
	CHECK(Classes::IsA(GuiClass("ArcHandles"), GuiClass("PVAdornment")));

	// A `UIFlexItem` is a component-style modifier and not a constraint - a
	// migrating script tells the two apart with exactly this pair.
	CHECK(Classes::IsA(GuiClass("UIFlexItem"), GuiClass("UIComponent")));
	CHECK(Classes::IsA(GuiClass("UIMask"), GuiClass("UIComponent")));
	CHECK_FALSE(Classes::IsA(GuiClass("UIFlexItem"), GuiClass("UIConstraint")));

	// **A collector is not a `GuiObject`**, which is the one relation people
	// assume and Roblox does not have. A `ScreenGui` has no `Position` and no
	// `BackgroundColor3`; if it derived from `GuiObject` it would have both and
	// the layout would try to resolve them against a parent it does not have.
	CHECK_FALSE(Classes::IsA(GuiClass("ScreenGui"), GuiClass("GuiObject")));

	// And a modifier is not a `GuiBase`. A `UIPadding` under a frame is a child
	// in the tree and must never be laid out as one.
	CHECK_FALSE(Classes::IsA(GuiClass("UIPadding"), GuiClass("GuiBase")));
}

TEST_CASE("the property surface exposes no controls without consumers", "[gui][registration]") {
	RegisterGuiClasses();
	Store store("gui_registration.supported_surface");

	const auto has = [&](std::string_view className, std::string_view propertyName) {
		const Entity instance = store.CreateInstance(GuiClass(className), className);
		for (const auto &property : store.PropertiesOf(instance)) {
			if (property.Name == Name(propertyName)) {
				return true;
			}
		}
		return false;
	};

	// These fields remain reserved in the raw components for format stability,
	// but none has an input, layout, or render consumer yet. Advertising them
	// would turn a successful write into a visible no-op.
	//
	// **The list is meant to shrink**, and every removal below records what
	// arrived to allow it. A property leaving this case is the only evidence
	// that "absent because the thing behind it is" was a statement about the
	// engine rather than a permanent excuse.
	CHECK_FALSE(has("TextButton", "Modal"));
	CHECK_FALSE(has("TextButton", "Selected"));

	// **`ScrollingEnabled` and `AutomaticCanvasSize` left this list at v0.18**,
	// which is what the list is for: the rule is that a property appears when
	// something reads it, and both are now read - the first by `gui::Router`,
	// which is what a wheel and a bar drag go through, and the second by
	// `gui::ContentArea`, which measures the content back into the canvas.
	CHECK(has("ScrollingFrame", "ScrollingEnabled"));
	CHECK(has("ScrollingFrame", "AutomaticCanvasSize"));

	// **The three `SelectionBox` members left this list at v0.17**, and what
	// unblocked them is worth recording because it was not what the entry that
	// held them back predicted. `docs/DEFERRED.md` said they needed "a triangle
	// path for adornments"; what was actually missing was any path at all -
	// `render::AdornmentGeometry` had no caller anywhere and no pass in the
	// engine drew a world-space line, so a `SelectionBox` drew nothing whatever
	// its properties said. `Editor::DrawAdornments` is the consumer, and the
	// surface arrived with it rather than after it.
	CHECK(has("SelectionBox", "LineThickness"));
	CHECK(has("SelectionBox", "SurfaceColor3"));
	CHECK(has("SelectionBox", "SurfaceTransparency"));
	CHECK(has("SelectionBox", "InteractionEnabled"));

	// A `SelectionSphere` shares the component, so it shares the three. Checked
	// because sharing is Roblox's arrangement rather than an implementation
	// detail: both are a `PVAdornment` with an outline and a surface.
	CHECK(has("SelectionSphere", "LineThickness"));
	CHECK(has("SelectionSphere", "SurfaceTransparency"));

	// And a handle still has none of them, which is the half that keeps the
	// rule honest: `gui::SelectionOutline` is on the two selection classes only,
	// so a grab target does not grow a surface it would draw as six slabs.
	CHECK_FALSE(has("BoxHandleAdornment", "LineThickness"));
	CHECK_FALSE(has("BoxHandleAdornment", "SurfaceTransparency"));

	// Neighbouring implemented controls stay present, so this cannot pass by
	// accidentally dropping the component or the whole class.
	CHECK(has("TextButton", "AutoButtonColor"));
	CHECK(has("ScrollingFrame", "CanvasPosition"));
	CHECK(has("SelectionBox", "Color3"));
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
	written.MaxVisible = 9;
	written.Rich = true;

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
	CHECK(read.MaxVisible == written.MaxVisible);
	CHECK(read.Rich == written.Rich);
}

TEST_CASE("a LabelPresentation round-trips separately from a legacy Label", "[gui][registration]") {
	RegisterGuiComponents();

	LabelPresentation written;
	written.LocalizationKey = Name("menu.greeting");

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<LabelPresentation>());
	ByteWriter writer;
	descriptor.Write(writer, &written, 1);

	LabelPresentation read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	CHECK(read.LocalizationKey == written.LocalizationKey);
}

TEST_CASE("localized label arguments round-trip through their independent component", "[gui][registration]") {
	RegisterGuiComponents();
	LabelLocalizationArguments written;
	written.Count = 3;
	written.Values[0] = {Name("name"), LocalizedArgumentType::String, "Ada"};
	written.Values[1] = {Name("total"), LocalizedArgumentType::Number, {}, 12.5};
	written.Values[2] = {Name("day"), LocalizedArgumentType::Date, {}, 0.0, 86400};

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<LabelLocalizationArguments>());
	ByteWriter writer;
	descriptor.Write(writer, &written, 1);
	LabelLocalizationArguments read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	REQUIRE_FALSE(reader.Failed());
	CHECK(read.Count == 3);
	CHECK(read.Values[0].Name == Name("name"));
	CHECK(read.Values[0].String == "Ada");
	CHECK(read.Values[1].Type == LocalizedArgumentType::Number);
	CHECK(read.Values[1].Number == 12.5);
	CHECK(read.Values[2].Type == LocalizedArgumentType::Date);
	CHECK(read.Values[2].UnixSeconds == 86400);
}

TEST_CASE(
	"authored style values preserve typed names and state through a save", "[gui][registration][style]"
) {
	RegisterGuiComponents();
	UITheme theme;
	REQUIRE(theme.Tokens.Set({Name("Panel"), StyleValue::FromColor({0.2f, 0.3f, 0.4f})}));
	ByteWriter themeBytes;
	const TypeDescriptor &themeDescriptor = Components::Describe(Components::Of<UITheme>());
	REQUIRE(themeDescriptor.Serialisable);
	themeDescriptor.Write(themeBytes, &theme, 1);
	UITheme restoredTheme;
	ByteReader themeReader(themeBytes.Bytes());
	themeDescriptor.Read(themeReader, &restoredTheme, 1);
	REQUIRE_FALSE(themeReader.Failed());
	REQUIRE(restoredTheme.Tokens.Find(Name("Panel")) != nullptr);
	CHECK(restoredTheme.Tokens.Find(Name("Panel"))->Color.G == 0.3f);

	StyleClass classes;
	REQUIRE(classes.Names.Add(Name("primary")));
	REQUIRE(classes.Names.Add(Name("compact")));
	ByteWriter classBytes;
	const TypeDescriptor &classDescriptor = Components::Describe(Components::Of<StyleClass>());
	classDescriptor.Write(classBytes, &classes, 1);
	StyleClass restoredClasses;
	ByteReader classReader(classBytes.Bytes());
	classDescriptor.Read(classReader, &restoredClasses, 1);
	REQUIRE_FALSE(classReader.Failed());
	REQUIRE(restoredClasses.Names.Names().size() == 2);
	CHECK(restoredClasses.Names.Names().back() == Name("compact"));

	UIStyle style;
	style.Rule.Class = Name("primary");
	style.Rule.State = StyleState::Hovered | StyleState::Focused;
	REQUIRE(style.Rule.Declarations.Set({Name("TextTransparency"), StyleValue::FromNumber(0.25f)}));
	ByteWriter styleBytes;
	const TypeDescriptor &styleDescriptor = Components::Describe(Components::Of<UIStyle>());
	styleDescriptor.Write(styleBytes, &style, 1);
	UIStyle restoredStyle;
	ByteReader styleReader(styleBytes.Bytes());
	styleDescriptor.Read(styleReader, &restoredStyle, 1);
	REQUIRE_FALSE(styleReader.Failed());
	CHECK(restoredStyle.Rule.Class == Name("primary"));
	CHECK(Includes(restoredStyle.Rule.State, StyleState::Hovered | StyleState::Focused));
	REQUIRE(restoredStyle.Rule.Declarations.Find(Name("TextTransparency")) != nullptr);
	CHECK(restoredStyle.Rule.Declarations.Find(Name("TextTransparency"))->Number == 0.25f);
	CHECK(Components::Describe(Components::Of<ResolvedStyle>()).Serialisable);

	StyleDirect direct;
	direct.Set(StyleDirectProperty::TextColor);
	ByteWriter directBytes;
	const TypeDescriptor &directDescriptor = Components::Describe(Components::Of<StyleDirect>());
	REQUIRE(directDescriptor.Serialisable);
	directDescriptor.Write(directBytes, &direct, 1);
	StyleDirect restoredDirect;
	ByteReader directReader(directBytes.Bytes());
	directDescriptor.Read(directReader, &restoredDirect, 1);
	CHECK(restoredDirect.Has(StyleDirectProperty::TextColor));
}

TEST_CASE("a GUI snapshot retains explicit default visual values", "[gui][registration][style]") {
	RegisterGuiClasses();
	Store source("gui_style_direct_snapshot_source");
	const Entity label = source.CreateInstance(GuiClass("TextLabel"), "Title");
	const engine::core::Color3 defaultColor = Label().Color;
	REQUIRE(source.SetProperty(label, Name("TextColor3"), &defaultColor, sizeof(defaultColor)));

	ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("gui_style_direct_snapshot_restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	const StyleDirect *direct = restored.Get<StyleDirect>(label);
	REQUIRE(direct != nullptr);
	CHECK(direct->Has(StyleDirectProperty::TextColor));
}

TEST_CASE("IME composition is omitted from GUI snapshots", "[gui][registration][typing]") {
	RegisterGuiClasses();
	Store source("gui_composition_snapshot_source");
	const Entity textBox = source.CreateInstance(GuiClass("TextBox"), "Entry");
	TextCompositionState composition;
	composition.Text = "candidate";
	composition.Revision = 7;
	source.Set(textBox, composition);

	const TypeDescriptor &serviceType = Components::Describe(Components::Of<GuiServiceState>());
	const TypeDescriptor &compositionType = Components::Describe(Components::Of<TextCompositionState>());
	CHECK(serviceType.Serialisable);
	CHECK(compositionType.Serialisable);

	ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("gui_composition_snapshot_restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	const TextCompositionState *restoredComposition = restored.Get<TextCompositionState>(textBox);
	CHECK((restoredComposition == nullptr || restoredComposition->Text.empty()));
}

TEST_CASE("a virtual collection persists its bounded source page", "[gui][registration]") {
	RegisterGuiComponents();
	Binding binding;
	binding.SourcePath = "Services/Score";
	binding.Attribute = Name("Value");
	binding.Target = Name("Text");
	binding.Fallback = "zero";
	const TypeDescriptor &bindingType = Components::Describe(Components::Of<Binding>());
	REQUIRE(bindingType.Serialisable);
	ByteWriter bindingWriter;
	bindingType.Write(bindingWriter, &binding, 1);
	Binding readBinding;
	ByteReader bindingReader(bindingWriter.Bytes());
	bindingType.Read(bindingReader, &readBinding, 1);
	CHECK(readBinding.SourcePath == binding.SourcePath);
	CHECK(readBinding.Attribute == binding.Attribute);
	CHECK(readBinding.Target == binding.Target);
	CHECK(readBinding.Fallback == binding.Fallback);

	VirtualCollection written;
	written.ItemCount = 1000000;
	written.Page.First = 800;
	VirtualRecord record;
	record.Key = "player";
	written.Page.Records.push_back(std::move(record));
	const TypeDescriptor &type = Components::Describe(Components::Of<VirtualCollection>());
	REQUIRE(type.Serialisable);
	CHECK(type.MaximumSerialisedBytes == VirtualCollection::MAXIMUM_PAGE_BYTES);

	ByteWriter writer;
	type.Write(writer, &written, 1);
	VirtualCollection restored;
	ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);
	CHECK(restored.ItemCount == written.ItemCount);
	CHECK(restored.Page.First == written.Page.First);
	REQUIRE(restored.Page.Records.size() == 1);
	CHECK(restored.Page.Records.front().Key == "player");

	CHECK(Components::Describe(Components::Of<BindingOutput>()).Serialisable);
	CHECK(Components::Describe(Components::Of<BindingDependency>()).Serialisable);
}

TEST_CASE(
	"a UIAnimation preserves its bounded typed tracks across a save", "[gui][registration][animation]"
) {
	RegisterGuiClasses();
	AnimationPlayback written;
	written.StartedAt = 42.5;
	written.Playing = false;
	written.Clip.Tween = engine::core::TweenInfo(
		2.0f, engine::core::EasingStyle::Sine, engine::core::EasingDirection::InOut, 2, true, 0.25f
	);
	PresentationTrack track;
	track.Property = PresentationProperty::Position;
	REQUIRE(track.Add(
		PresentationKey{0.0f, PresentationValue::FromUDim2(engine::core::UDim2{0.0f, 1.0f, 0.0f, 2.0f})}
	));
	REQUIRE(track.Add(
		PresentationKey{1.0f, PresentationValue::FromUDim2(engine::core::UDim2{1.0f, 3.0f, 1.0f, 4.0f})}
	));
	REQUIRE(written.Clip.AddTrack(track));
	REQUIRE(written.Clip.AddMarker(AnimationMarker{Name("half"), 0.5f}));

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<AnimationPlayback>());
	REQUIRE(descriptor.Serialisable);
	CHECK(descriptor.MaximumSerialisedBytes > 1024);
	ByteWriter writer;
	descriptor.Write(writer, &written, 1);
	AnimationPlayback restored;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &restored, 1);
	CHECK_FALSE(reader.Failed());
	CHECK(restored.StartedAt == -1.0);
	CHECK(restored.Playing == written.Playing);
	CHECK(restored.Clip.Tween == written.Clip.Tween);
	REQUIRE(restored.Clip.Tracks().size() == 1);
	CHECK(restored.Clip.Tracks().front().Property == PresentationProperty::Position);
	CHECK(
		restored.Clip.Tracks().front().Keys().back().Value.UDim2 ==
		engine::core::UDim2{1.0f, 3.0f, 1.0f, 4.0f}
	);
	REQUIRE(restored.Clip.Markers().size() == 1);
	CHECK(restored.Clip.Markers().front().Name == Name("half"));

	// Marker names are part of the declared maximum serialised size. Reject a
	// longer one before interning it, so hostile bytes cannot turn the bound
	// into process-lifetime Name storage.
	ByteWriter hostile;
	hostile.WriteBool(true);
	hostile.WriteFloat(1.0f);
	hostile.WriteFloat(0.0f);
	hostile.WriteInt32(0);
	hostile.WriteUInt8(static_cast<uint8_t>(engine::core::EasingStyle::Linear));
	hostile.WriteUInt8(static_cast<uint8_t>(engine::core::EasingDirection::In));
	hostile.WriteBool(false);
	hostile.WriteUInt32(0);
	hostile.WriteUInt32(1);
	hostile.WriteString(std::string(UIAnimation::MAXIMUM_MARKER_NAME_BYTES + 1, 'x'));
	hostile.WriteFloat(0.5f);
	AnimationPlayback refused;
	ByteReader hostileReader(hostile.Bytes());
	descriptor.Read(hostileReader, &refused, 1);
	CHECK(hostileReader.Failed());
	restored.Playing = true;

	Store store("gui_registration.animation_restart");
	const Entity modifier = store.CreateInstance(GuiClass("UIAnimation"), "UIAnimation");
	store.Set(modifier, restored);
	AdvancePresentationAnimations(store, 500.0);
	const AnimationPlayback *restarted = store.Get<AnimationPlayback>(modifier);
	const PresentationState *state = store.Get<PresentationState>(modifier);
	REQUIRE(restarted != nullptr);
	REQUIRE(state != nullptr);
	CHECK(restarted->StartedAt == 500.0);
	const PresentationOverride *position = state->Overrides.Find(PresentationProperty::Position);
	REQUIRE(position != nullptr);
	CHECK(position->Value.UDim2 == engine::core::UDim2{0.0f, 1.0f, 0.0f, 2.0f});
	CHECK(Components::Describe(Components::Of<PresentationState>()).Serialisable);
}

TEST_CASE("local GUI facets restore empty after a snapshot", "[gui][registration][snapshot]") {
	RegisterGuiComponents();
	Store source("gui_transient_snapshot_source");
	const Entity instance = source.Create();

	BindingOutput output;
	output.Value = "old result";
	output.Valid = true;
	output.Failure = BindingFailure::MissingAttribute;
	output.SourceRevision = 7;
	output.EvaluationCount = 9;
	source.Set(instance, output);
	source.Set(instance, BindingDependency{Entity{22}, 5, 7});
	source.Set(instance, VirtualFocusState{Entity{22}, "row-7", 7});
	source.Set(instance, VirtualAnchorState{7, "row-7", 14.0f});
	PresentationState presentation;
	presentation.Active = true;
	presentation.Moving = true;
	source.Set(instance, presentation);
	ResolvedStyle style;
	style.State = StyleState::Hovered;
	source.Set(instance, style);

	ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("gui_transient_snapshot_restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const BindingOutput *restoredOutput = restored.Get<BindingOutput>(instance);
	const BindingDependency *restoredDependency = restored.Get<BindingDependency>(instance);
	const VirtualFocusState *restoredFocus = restored.Get<VirtualFocusState>(instance);
	const VirtualAnchorState *restoredAnchor = restored.Get<VirtualAnchorState>(instance);
	const PresentationState *restoredPresentation = restored.Get<PresentationState>(instance);
	const ResolvedStyle *restoredStyle = restored.Get<ResolvedStyle>(instance);
	REQUIRE(restoredOutput != nullptr);
	REQUIRE(restoredDependency != nullptr);
	REQUIRE(restoredFocus != nullptr);
	REQUIRE(restoredAnchor != nullptr);
	REQUIRE(restoredPresentation != nullptr);
	REQUIRE(restoredStyle != nullptr);
	CHECK(restoredOutput->Value.empty());
	CHECK_FALSE(restoredOutput->Valid);
	CHECK(restoredDependency->Source == Entity{});
	CHECK(restoredFocus->Collection == Entity{});
	CHECK(restoredFocus->Key.empty());
	CHECK(restoredAnchor->Revision == std::numeric_limits<uint64_t>::max());
	CHECK(restoredAnchor->Key.empty());
	CHECK_FALSE(restoredPresentation->Active);
	CHECK_FALSE(restoredPresentation->Moving);
	CHECK(restoredStyle->State == StyleState::None);
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
	written.Shader = Name("toon");
	written.HoverImage = Name("rbxasset://textures/panel-hover.png");
	written.PressedImage = Name("rbxasset://textures/panel-press.png");
	written.Resample = ResampleMode::Pixelated;

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
	CHECK(read.Shader == written.Shader);
	CHECK(read.HoverImage == written.HoverImage);
	CHECK(read.PressedImage == written.PressedImage);
	CHECK(read.Resample == written.Resample);
}

TEST_CASE("node canvas execution settings and port limits survive a save", "[gui][registration]") {
	RegisterGuiComponents();

	NodeCanvasNode writtenNode;
	writtenNode.Id = Name("passthrough");
	writtenNode.Type = Name("logic.Reroute");
	writtenNode.Title = "Pass through";
	writtenNode.Enabled = false;
	writtenNode.BypassMode = NodeBypassMode::Bypass;
	writtenNode.BypassInput = Name("value");
	writtenNode.BypassOutput = Name("result");
	writtenNode.MinimumSize = {112.0f, 56.0f};
	writtenNode.Resizable = false;
	writtenNode.InputLayout = InputPortLayout::Squash;

	const TypeDescriptor &nodeType = Components::Describe(Components::Of<NodeCanvasNode>());
	ByteWriter nodeWriter;
	nodeType.Write(nodeWriter, &writtenNode, 1);
	NodeCanvasNode readNode;
	ByteReader nodeReader(nodeWriter.Bytes());
	nodeType.Read(nodeReader, &readNode, 1);

	CHECK(readNode.Id == writtenNode.Id);
	CHECK(readNode.Type == writtenNode.Type);
	CHECK(readNode.Title == writtenNode.Title);
	CHECK(readNode.Enabled == writtenNode.Enabled);
	CHECK(readNode.BypassMode == writtenNode.BypassMode);
	CHECK(readNode.BypassInput == writtenNode.BypassInput);
	CHECK(readNode.BypassOutput == writtenNode.BypassOutput);
	CHECK(readNode.MinimumSize == writtenNode.MinimumSize);
	CHECK(readNode.Resizable == writtenNode.Resizable);
	CHECK(readNode.InputLayout == writtenNode.InputLayout);

	NodeCanvasPort writtenPort;
	writtenPort.Id = Name("values");
	writtenPort.ValueType = Name("any");
	writtenPort.Direction = NodePortDirection::Input;
	writtenPort.Edge = NodePortEdge::Corner;
	writtenPort.MaxConnections = 3;

	const TypeDescriptor &portType = Components::Describe(Components::Of<NodeCanvasPort>());
	ByteWriter portWriter;
	portType.Write(portWriter, &writtenPort, 1);
	NodeCanvasPort readPort;
	ByteReader portReader(portWriter.Bytes());
	portType.Read(portReader, &readPort, 1);

	CHECK(readPort.Id == writtenPort.Id);
	CHECK(readPort.ValueType == writtenPort.ValueType);
	CHECK(readPort.Direction == writtenPort.Direction);
	CHECK(readPort.Edge == writtenPort.Edge);
	CHECK(readPort.MaxConnections == writtenPort.MaxConnections);

	NodeCanvasLink writtenLink;
	writtenLink.FromNode = Name("remap");
	writtenLink.FromPort = Name("value");
	writtenLink.FromDirection = NodePortDirection::Output;
	writtenLink.ToNode = Name("display");
	writtenLink.ToPort = Name("value");
	writtenLink.ToDirection = NodePortDirection::Input;

	const TypeDescriptor &linkType = Components::Describe(Components::Of<NodeCanvasLink>());
	ByteWriter linkWriter;
	linkType.Write(linkWriter, &writtenLink, 1);
	NodeCanvasLink readLink;
	ByteReader linkReader(linkWriter.Bytes());
	linkType.Read(linkReader, &readLink, 1);

	CHECK(readLink.FromNode == writtenLink.FromNode);
	CHECK(readLink.FromPort == writtenLink.FromPort);
	CHECK(readLink.FromDirection == writtenLink.FromDirection);
	CHECK(readLink.ToNode == writtenLink.ToNode);
	CHECK(readLink.ToPort == writtenLink.ToPort);
	CHECK(readLink.ToDirection == writtenLink.ToDirection);
}

TEST_CASE("a text box's caret does not cross a save", "[gui][registration]") {
	RegisterGuiComponents();

	// Where somebody's cursor is is not a fact about the game. A save that
	// restored a text box mid-edit would be restoring a session, and a replica
	// that received one would move the local player's caret.
	Entry written;
	written.PlaceholderText = "type here";
	written.Password = true;
	written.CursorPosition = 4;
	written.SelectionStart = 2;

	const TypeDescriptor &descriptor = Components::Describe(Components::Of<Entry>());

	ByteWriter writer;
	descriptor.Write(writer, &written, 1);

	Entry read;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &read, 1);

	CHECK(read.PlaceholderText == written.PlaceholderText);
	CHECK(read.Password);
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
	// the property surface - the same path a script takes - and the registry
	// must not have moved at all.
	RegisterGuiClasses();

	Store store("gui_registration.owned_text");
	const Entity label = store.CreateInstance(GuiClass("TextLabel"), "Score");

	// One write first, so any interning the *path* does - the property name,
	// the class name, a lazily built table - has already happened and is not
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
	// what makes it an integer comparison everywhere downstream - the same trade
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

TEST_CASE("every interface component obeys the serialisation rules", "[gui][registration]") {
	// **Thirteen of these were wrong at once**, which is what makes the sweep
	// worth having over a list: every one had padding under a raw writer, and
	// four of them were demonstrably putting bytes nobody wrote into a save.
	// None of it was visible from any single component's own test.
	//
	// `engine.ecs.invariants` is where the rules live and where each is proved
	// to fire. This asks them about this module.
	RegisterGuiComponents();

	CHECK(engine::ecs::Describe(engine::ecs::AuditComponents("gui.")) == "");
}

// **The widest row in the interface set, and what it was actually carrying.**
//
// `gui.Gradient` is 672 bytes and 656 of them are two sequences of twenty
// keypoint slots. Until v0.19 it had no hand-written pair, so the generated
// writer copied the object representation and every gradient wrote all forty
// slots into every save and every replication delta whatever `Count` said - and
// `gui.Gradient` is under the shared prefix, so it crossed at full width to
// every client.
//
// A `UIGradient` an author never touched is the two-stop default. This measures
// what that costs now against what it cost then.
TEST_CASE("a gradient writes the stops it has and not the ones it could", "[gui][registration]") {
	engine::gui::RegisterGuiComponents();

	const TypeDescriptor &type = Components::Describe(Components::Find(Name("gui.Gradient")));
	REQUIRE(type.Serialisable);

	const auto written = [&type](const engine::gui::Gradient &gradient) {
		ByteWriter writer;
		type.Write(writer, &gradient, 1);
		return writer.Size();
	};

	// The default: two colour stops and two transparency stops. Four bytes of
	// count and four sixteen-byte colour stops' worth... written out, it is
	// 4 + 2*16 for the ramp, 4 + 2*12 for the curve, and 8 + 4 + 1 for the
	// offset, rotation and flag: **77 bytes against a 672-byte row.**
	const engine::gui::Gradient plain;
	CHECK(written(plain) == 77);
	CHECK(written(plain) < sizeof(engine::gui::Gradient) / 8);

	// And a ramp somebody actually authored costs more, which is the half that
	// says the count is being honoured rather than a constant written down.
	engine::gui::Gradient rich;
	REQUIRE(rich.Color.Add(engine::core::ColorKeypoint{0.5f, engine::core::Color3{1.0f, 0.0f, 0.0f}}));
	CHECK(written(rich) > written(plain));

	// The round trip, over a ramp with a stop in the middle and an envelope on
	// the curve - the field `core::NumberSequence` carries and nothing samples,
	// which still has to come back or a re-save loses it.
	engine::gui::Gradient authored;
	authored.Color = engine::core::ColorSequence{
		engine::core::Color3{1.0f, 0.0f, 0.0f}, engine::core::Color3{0.0f, 0.0f, 1.0f}
	};
	REQUIRE(authored.Color.Add(engine::core::ColorKeypoint{0.25f, engine::core::Color3{0.0f, 1.0f, 0.0f}}));
	authored.Transparency = engine::core::NumberSequence{0.0f, 1.0f};
	REQUIRE(authored.Transparency.Add(engine::core::NumberKeypoint{0.5f, 0.25f, 0.125f}));
	authored.Offset = engine::core::Vector2{0.25f, -0.75f};
	authored.Rotation = 45.0f;
	authored.Enabled = false;

	ByteWriter writer;
	type.Write(writer, &authored, 1);
	ByteReader reader(writer.Bytes());

	engine::gui::Gradient restored;
	type.Read(reader, &restored, 1);

	REQUIRE(restored.Color.Count == authored.Color.Count);
	for (uint32_t stop = 0; stop < authored.Color.Count; stop++) {
		INFO("colour stop " << stop);
		CHECK(restored.Color.Keypoints[stop].Time == authored.Color.Keypoints[stop].Time);
		CHECK(restored.Color.Keypoints[stop].Value.R == authored.Color.Keypoints[stop].Value.R);
		CHECK(restored.Color.Keypoints[stop].Value.G == authored.Color.Keypoints[stop].Value.G);
		CHECK(restored.Color.Keypoints[stop].Value.B == authored.Color.Keypoints[stop].Value.B);
	}

	REQUIRE(restored.Transparency.Count == authored.Transparency.Count);
	for (uint32_t stop = 0; stop < authored.Transparency.Count; stop++) {
		INFO("transparency stop " << stop);
		CHECK(restored.Transparency.Keypoints[stop].Time == authored.Transparency.Keypoints[stop].Time);
		CHECK(restored.Transparency.Keypoints[stop].Value == authored.Transparency.Keypoints[stop].Value);
		CHECK(
			restored.Transparency.Keypoints[stop].Envelope == authored.Transparency.Keypoints[stop].Envelope
		);
	}

	CHECK(restored.Offset.X == authored.Offset.X);
	CHECK(restored.Offset.Y == authored.Offset.Y);
	CHECK(restored.Rotation == authored.Rotation);
	CHECK(restored.Enabled == authored.Enabled);

	// **A count past the capacity is a file claiming something the type cannot
	// hold**, and `Sequence::Add` refuses rather than writing past the array.
	ByteWriter hostile;
	hostile.WriteUInt32(9999);
	engine::gui::Gradient decoded;
	ByteReader attack(hostile.Bytes());
	type.Read(attack, &decoded, 1);
	CHECK(decoded.Color.Count <= engine::core::SEQUENCE_CAPACITY);
}
