#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::gui {

	namespace {
		using ecs::Classes;
		using ecs::ClassId;
		using ecs::Components;
		using ecs::ComponentSet;
		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;

		// --- naming the enum sets --------------------------------------------
		//
		// One function per set, interned once. `scene`'s `NormalIdEnum` carries
		// the measurement this follows: constructing a `core::Name` from a
		// literal takes the process-wide registry's mutex and hashes a string,
		// and a property getter is read every frame by an immediate-mode
		// properties panel — which is that loop.
		//
		// **A function template keyed on the C++ enum, not a parameter.** The
		// generated conversions below are captureless function pointers, so
		// they cannot close over a name; keying on the type is what lets one
		// template generate the getter and setter for all nineteen sets.

		template <class E> const core::Name &EnumNameOf();

#define GUI_ENUM_NAME(Type, Spelling)                                                                        \
	template <> const core::Name &EnumNameOf<Type>() {                                                       \
		static const core::Name name(Spelling);                                                              \
		return name;                                                                                         \
	}

		GUI_ENUM_NAME(SizeConstraint, "SizeConstraint")
		GUI_ENUM_NAME(AutomaticSize, "AutomaticSize")
		GUI_ENUM_NAME(BorderMode, "BorderMode")
		GUI_ENUM_NAME(ScaleType, "ScaleType")
		GUI_ENUM_NAME(TextXAlignment, "TextXAlignment")
		GUI_ENUM_NAME(TextYAlignment, "TextYAlignment")
		GUI_ENUM_NAME(TextTruncate, "TextTruncate")
		GUI_ENUM_NAME(FontFace, "Font")
		GUI_ENUM_NAME(FillDirection, "FillDirection")
		GUI_ENUM_NAME(HorizontalAlignment, "HorizontalAlignment")
		GUI_ENUM_NAME(VerticalAlignment, "VerticalAlignment")
		GUI_ENUM_NAME(SortOrder, "SortOrder")
		GUI_ENUM_NAME(StartCorner, "StartCorner")
		GUI_ENUM_NAME(AspectType, "AspectType")
		GUI_ENUM_NAME(DominantAxis, "DominantAxis")
		GUI_ENUM_NAME(ScrollingDirection, "ScrollingDirection")
		GUI_ENUM_NAME(ZIndexBehavior, "ZIndexBehavior")
		GUI_ENUM_NAME(SurfaceSizingMode, "SurfaceSizingMode")
		GUI_ENUM_NAME(Face, "NormalId")

#undef GUI_ENUM_NAME

		// The stored value of the *first* member of a set.
		//
		// Zero for every set but one. `ScrollingDirection` is `X = 1`, `Y = 2`,
		// `XY = 3` in Roblox — a bit pair rather than a counter — and the
		// ordinal is the format, so it is kept rather than tidied to start at
		// zero. `EnumTable` numbers members from zero whatever they mean, so
		// the offset is applied here, once, in both directions.
		template <class E> constexpr uint8_t EnumOrigin() {
			return 0;
		}

		template <> constexpr uint8_t EnumOrigin<ScrollingDirection>() {
			return 1;
		}

		// How many members a set has. Only needed to walk the range when
		// registering, so it lives beside the registration rather than in the
		// header — a count in `Enums.hpp` would be a third place the list lives.
		template <class E> constexpr size_t EnumCount();

#define GUI_ENUM_COUNT(Type, Number)                                                                         \
	template <> constexpr size_t EnumCount<Type>() {                                                         \
		return Number;                                                                                       \
	}

		GUI_ENUM_COUNT(SizeConstraint, 3)
		GUI_ENUM_COUNT(AutomaticSize, 4)
		GUI_ENUM_COUNT(BorderMode, 3)
		GUI_ENUM_COUNT(ScaleType, 5)
		GUI_ENUM_COUNT(TextXAlignment, 3)
		GUI_ENUM_COUNT(TextYAlignment, 3)
		GUI_ENUM_COUNT(TextTruncate, 2)
		GUI_ENUM_COUNT(FontFace, 4)
		GUI_ENUM_COUNT(FillDirection, 2)
		GUI_ENUM_COUNT(HorizontalAlignment, 3)
		GUI_ENUM_COUNT(VerticalAlignment, 3)
		GUI_ENUM_COUNT(SortOrder, 3)
		GUI_ENUM_COUNT(StartCorner, 4)
		GUI_ENUM_COUNT(AspectType, 2)
		GUI_ENUM_COUNT(DominantAxis, 2)
		GUI_ENUM_COUNT(ScrollingDirection, 3)
		GUI_ENUM_COUNT(ZIndexBehavior, 2)
		GUI_ENUM_COUNT(SurfaceSizingMode, 2)
		GUI_ENUM_COUNT(Face, 6)

#undef GUI_ENUM_COUNT

		// Registers one set by walking its own range and asking `Describe`.
		//
		// **Generated rather than typed out beside the enum**, which is the
		// difference between one declaration and two that agree until they do
		// not. `scene::RegisterTree` does the same for `NormalId` and gives the
		// reason: the ordinal is the storage, so a literal list here would be a
		// second place the order lives.
		template <class E> void RegisterEnum() {
			std::array<std::string_view, EnumCount<E>()> members{};
			for (size_t index = 0; index < members.size(); index++) {
				members[index] = Describe(static_cast<E>(index + EnumOrigin<E>()));
			}
			ecs::EnumTable::Register(EnumNameOf<E>().Text(), members);
		}

		// --- the generated enum property -------------------------------------

		template <class T> struct MemberOf;
		template <class C, class M> struct MemberOf<M C::*> {
			using Component = C;
			using Value = M;
		};

		// An enum-typed field, as a property whose value is a checked name.
		//
		// `Classes::Property` cannot express this: its generated conversion is
		// typed by `TypeOf<T>`, which answers `Opaque` for anything it does not
		// know — so every one of these would have been a property nothing could
		// read. This is the same generated-field idea with the name lookup
		// folded in, and it is a template for the reason `Property` is one: the
		// member pointer as a template argument is what keeps the conversion a
		// plain function pointer with nothing captured.
		//
		// @tparam Member A pointer to an enum-typed field of a registered
		//                component.
		// @param name    The property's name, as a script spells it.
		// @return The descriptor, ready for `Classes::Computed`.
		template <auto Member> PropertyDescriptor EnumField(std::string_view name) {
			using Traits = MemberOf<decltype(Member)>;
			using Component = typename Traits::Component;
			using Value = typename Traits::Value;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Enum;
			property.EnumName = EnumNameOf<Value>();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Field;
			property.Reads = &ComponentSet::Intern({Components::Of<Component>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Component *component = store.Get<Component>(instance);
				if (component == nullptr) {
					return false;
				}
				const auto stored = static_cast<size_t>(component->*Member);
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(EnumNameOf<Value>(), stored - EnumOrigin<Value>());
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						EnumNameOf<Value>(), *static_cast<const core::Name *>(value), ordinal
					)) {
					// Refused where it was written. That is the whole of what
					// `PropertyType::Enum` buys over `Name`: `label.Font =
					// "Bodl"` is an error rather than a label silently drawn in
					// the default face.
					return false;
				}

				Component *component = store.GetMutable<Component>(instance);
				if (component == nullptr) {
					return false;
				}
				component->*Member = static_cast<Value>(ordinal + EnumOrigin<Value>());
				return true;
			};

			return property;
		}

		// A field of `Resolved`, exposed read-only.
		//
		// **Read-only is the contract and not a convenience.** `AbsoluteSize`
		// is what the layout pass decided; a script that assigned to it would
		// be overwritten on the very next frame, and a property that silently
		// discards a write is worse than one that refuses it. Roblox's are
		// read-only for the same reason.
		//
		// @tparam Member A pointer to a field of `Resolved`.
		// @param name    The property's name.
		// @return The descriptor.
		// The property type of a value, for the read-only fields below.
		//
		// `Classes::TypeOf` answers exactly this and is private, which is
		// right: it exists to type a *generated* conversion, and a public one
		// would be an invitation to switch on it. Two cases are all this file
		// needs.
		template <class T> constexpr PropertyType TypeOfValue() {
			if constexpr (std::is_same_v<T, core::Vector2>) {
				return PropertyType::Vector2;
			} else {
				static_assert(std::is_same_v<T, float>, "add a case for this type");
				return PropertyType::Float;
			}
		}

		template <auto Member> PropertyDescriptor ResolvedField(std::string_view name) {
			using Value = typename MemberOf<decltype(Member)>::Value;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = TypeOfValue<Value>();
			property.Size = sizeof(Value);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ComponentSet::Intern({Components::Of<Resolved>()});
			property.Writes = &ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Resolved *resolved = store.Get<Resolved>(instance);
				if (resolved == nullptr) {
					return false;
				}
				*static_cast<Value *>(out) = resolved->*Member;
				return true;
			};

			property.Set = [](ecs::Store &, ecs::Entity, const void *) -> bool { return false; };

			return property;
		}

		// Every class this module registers, in registration order.
		//
		// **One list, read by three callers** — the registration below, the
		// `GuiClassNames` accessor a palette and the manifest use, and
		// `gui/tests/Registration.cpp`. A class added to the tree and not to
		// this array fails that test, which is what makes the list a contract
		// rather than documentation.
		constexpr std::string_view CLASS_NAMES[] = {
			"GuiBase",
			"GuiBase2d",
			"GuiObject",
			"Frame",
			"CanvasGroup",
			"ScrollingFrame",
			"GuiButton",
			"TextButton",
			"ImageButton",
			"GuiLabel",
			"TextLabel",
			"ImageLabel",
			"TextBox",
			"ViewportFrame",
			"LayerCollector",
			"ScreenGui",
			"SurfaceGui",
			"BillboardGui",
			"PluginGui",
			"DockWidgetPluginGui",
			"UIBase",
			"UIComponent",
			"UILayout",
			"UIListLayout",
			"UIGridLayout",
			"UIConstraint",
			"UIAspectRatioConstraint",
			"UISizeConstraint",
			"UITextSizeConstraint",
			"UIPadding",
			"UICorner",
			"UIStroke",
			"UIScale",
			"GuiService",

			// The 3D branch. `GuiBase3d` and `PVAdornment` are abstract in
			// Roblox and are listed because a script may `:IsA` either.
			"GuiBase3d",
			"PVAdornment",
			"SelectionBox",
			"SelectionSphere",
			"HandleAdornment",
			"BoxHandleAdornment",
			"SphereHandleAdornment",
			"CylinderHandleAdornment",
			"LineHandleAdornment",
			"Handles",
			"ArcHandles",
		};

		// The tree, built once for the process.
		//
		// A function-local static in `RegisterGuiClasses`, exactly as
		// `scene::RegisterTree` is: the tree exists before the first caller
		// reads an id from it and cannot be registered twice.
		ClassId BuildTree() {
			RegisterGuiComponents();

			RegisterEnum<SizeConstraint>();
			RegisterEnum<AutomaticSize>();
			RegisterEnum<BorderMode>();
			RegisterEnum<ScaleType>();
			RegisterEnum<TextXAlignment>();
			RegisterEnum<TextYAlignment>();
			RegisterEnum<TextTruncate>();
			RegisterEnum<FontFace>();
			RegisterEnum<FillDirection>();
			RegisterEnum<HorizontalAlignment>();
			RegisterEnum<VerticalAlignment>();
			RegisterEnum<SortOrder>();
			RegisterEnum<StartCorner>();
			RegisterEnum<AspectType>();
			RegisterEnum<DominantAxis>();
			RegisterEnum<ScrollingDirection>();
			RegisterEnum<ZIndexBehavior>();
			RegisterEnum<SurfaceSizingMode>();

			// **The one set this module shares with `scene`.** Both register
			// `NormalId` and `EnumTable` takes the second declaration as
			// agreement — which is legal exactly as long as the orders match,
			// since the ordinal is what a game file carries. `Enums.hpp` says
			// so at the declaration and `gui/tests/Enums.cpp` pins it.
			RegisterEnum<Face>();

			const ClassId instance = Classes::RegisterInstanceRoot();

			// --- the 2D branch -----------------------------------------------
			//
			// `GuiBase` and `GuiBase2d` add no components of their own, which
			// is Roblox's shape and is worth keeping rather than collapsing:
			// `GuiBase3d` and the adornments hang off `GuiBase` when they
			// arrive, and a tree that had flattened the two would have to grow
			// the split back at exactly the point somebody is adding a feature.
			const ClassId guiBase = Classes::Register("GuiBase", instance, {});

			// **A service, so it hangs off `Instance` rather than off
			// `GuiBase`.** It is not a thing that draws — it is the thing that
			// *owns the selection*, which is what finally gives
			// `GuiObject::Selectable` a reader. `scene`'s services sit at the
			// root the same way, and `GetService` finds either by name.
			const std::array guiServiceState{Components::Of<GuiServiceState>()};
			const ClassId guiService = Classes::Register("GuiService", instance, guiServiceState);

			// --- the 3D branch -----------------------------------------------
			//
			// **Hung off `GuiBase`, which is what that class was kept for.** The
			// comment above said so when the 2D branch went in: `GuiBase` and
			// `GuiBase2d` add no components of their own and a tree that had
			// collapsed the two "would have to grow the split back at exactly
			// the point somebody is adding a feature". This is that point.
			//
			// An adornment is a description rather than a drawing — see
			// `Adornment` — so what is registered here is what to outline and
			// how, and nothing that resolves it into geometry.
			const ClassId guiBase3d = Classes::Register("GuiBase3d", guiBase, {});

			// `PVAdornment` is Roblox's name for "an adornment about a
			// `BasePart`", and the `Adornee` lives here rather than on
			// `GuiBase3d` because that is where Roblox puts it.
			const std::array adornment{Components::Of<Adornment>()};
			const ClassId pvAdornment = Classes::Register("PVAdornment", guiBase3d, adornment);

			const std::array outline{Components::Of<SelectionOutline>()};
			const ClassId selectionBox = Classes::Register("SelectionBox", pvAdornment, outline);
			const ClassId selectionSphere = Classes::Register("SelectionSphere", pvAdornment, outline);

			// A handle is an adornment with a shape and an offset. The four
			// leaves below differ only in what a drawer makes of `HandleShape`,
			// which is why they add no components of their own — the same shape
			// `Frame` and `CanvasGroup` have on the 2D side.
			const std::array handle{Components::Of<HandleShape>()};
			const ClassId handleAdornment = Classes::Register("HandleAdornment", pvAdornment, handle);

			const ClassId boxHandle = Classes::Register("BoxHandleAdornment", handleAdornment, {});
			const ClassId sphereHandle = Classes::Register("SphereHandleAdornment", handleAdornment, {});
			const ClassId cylinderHandle = Classes::Register("CylinderHandleAdornment", handleAdornment, {});
			const ClassId lineHandle = Classes::Register("LineHandleAdornment", handleAdornment, {});

			// `Handles` and `ArcHandles` are the draggable ones — the editor's
			// move and rotate gizmos. They carry the same `Adornment` their
			// siblings do and differ in what a drawer offers to grab, which is
			// the drawer's business rather than the tree's.
			const ClassId handles = Classes::Register("Handles", pvAdornment, {});
			const ClassId arcHandles = Classes::Register("ArcHandles", pvAdornment, {});

			// `Resolved` arrives here because both halves below need it: a
			// `LayerCollector` has an absolute rectangle just as an element
			// does, and the layout pass writes both.
			const std::array base2d{Components::Of<Resolved>()};
			const ClassId guiBase2d = Classes::Register("GuiBase2d", guiBase, base2d);

			const std::array object{Components::Of<Element>(), Components::Of<Background>()};
			const ClassId guiObject = Classes::Register("GuiObject", guiBase2d, object);

			const ClassId frame = Classes::Register("Frame", guiObject, {});

			const std::array group{Components::Of<Group>()};
			const ClassId canvasGroup = Classes::Register("CanvasGroup", frame, group);

			const std::array scrolling{Components::Of<Scrolling>()};
			const ClassId scrollingFrame = Classes::Register("ScrollingFrame", frame, scrolling);

			const std::array button{Components::Of<Button>()};
			const ClassId guiButton = Classes::Register("GuiButton", guiObject, button);

			const std::array label{Components::Of<Label>()};
			const std::array picture{Components::Of<Picture>()};

			const ClassId textButton = Classes::Register("TextButton", guiButton, label);
			const ClassId imageButton = Classes::Register("ImageButton", guiButton, picture);

			const ClassId guiLabel = Classes::Register("GuiLabel", guiObject, {});
			const ClassId textLabel = Classes::Register("TextLabel", guiLabel, label);
			const ClassId imageLabel = Classes::Register("ImageLabel", guiLabel, picture);

			// A text box is a label you can type into, so it carries both.
			const std::array entry{Components::Of<Label>(), Components::Of<Entry>()};
			const ClassId textBox = Classes::Register("TextBox", guiObject, entry);

			const std::array viewport{Components::Of<Viewport>()};
			const ClassId viewportFrame = Classes::Register("ViewportFrame", guiObject, viewport);

			// --- the collectors ----------------------------------------------

			const std::array collector{Components::Of<Layer>(), Components::Of<Canvas>()};
			const ClassId layerCollector = Classes::Register("LayerCollector", guiBase2d, collector);

			const ClassId screenGui = Classes::Register("ScreenGui", layerCollector, {});

			const std::array surface{Components::Of<Surface>()};
			const ClassId surfaceGui = Classes::Register("SurfaceGui", layerCollector, surface);

			const std::array billboard{Components::Of<Billboard>()};
			const ClassId billboardGui = Classes::Register("BillboardGui", layerCollector, billboard);

			// **Registered and drawn by nothing, and that is not the same as a
			// shim.** A `PluginGui` is a collector whose canvas is a *host
			// window*, and this engine's editor is Dear ImGui until the tree
			// can draw a property grid — so there is no window to be its
			// canvas yet. It is here because `DockWidgetPluginGui` is the
			// class the studio's own panels will be authored as, and its place
			// in the tree is what the last step of the v0.8 plan builds on.
			//
			// The distinction the roadmap draws is between a class that
			// *looks* present and one that is honestly incomplete: a
			// `PluginGui` produces no canvas, so nothing under it lays out and
			// nothing under it is silently discarded.
			const ClassId pluginGui = Classes::Register("PluginGui", layerCollector, {});
			const ClassId dockWidget = Classes::Register("DockWidgetPluginGui", pluginGui, {});

			// --- the modifiers -----------------------------------------------

			const ClassId uiBase = Classes::Register("UIBase", instance, {});
			const ClassId uiComponent = Classes::Register("UIComponent", uiBase, {});
			const ClassId uiLayout = Classes::Register("UILayout", uiComponent, {});

			const std::array listLayout{Components::Of<ListLayout>()};
			const ClassId uiListLayout = Classes::Register("UIListLayout", uiLayout, listLayout);

			const std::array gridLayout{Components::Of<GridLayout>()};
			const ClassId uiGridLayout = Classes::Register("UIGridLayout", uiLayout, gridLayout);

			const ClassId uiConstraint = Classes::Register("UIConstraint", uiComponent, {});

			const std::array aspect{Components::Of<AspectRatio>()};
			const ClassId uiAspect = Classes::Register("UIAspectRatioConstraint", uiConstraint, aspect);

			const std::array sizeLimits{Components::Of<SizeLimits>()};
			const ClassId uiSize = Classes::Register("UISizeConstraint", uiConstraint, sizeLimits);

			const std::array textLimits{Components::Of<TextSizeLimits>()};
			const ClassId uiTextSize = Classes::Register("UITextSizeConstraint", uiConstraint, textLimits);

			const std::array padding{Components::Of<Padding>()};
			const ClassId uiPadding = Classes::Register("UIPadding", uiComponent, padding);

			const std::array corner{Components::Of<Corner>()};
			const ClassId uiCorner = Classes::Register("UICorner", uiComponent, corner);

			const std::array stroke{Components::Of<Stroke>()};
			const ClassId uiStroke = Classes::Register("UIStroke", uiComponent, stroke);

			const std::array scale{Components::Of<Scale>()};
			const ClassId uiScale = Classes::Register("UIScale", uiComponent, scale);

			// --- the property surface ----------------------------------------
			//
			// Each declared on the class that first holds what it projects, so
			// a derived class inherits it and `Classes` merges base-first.
			// Declaring them all on the leaves would work today and would be
			// wrong the moment a second subclass exists — which, in this tree,
			// is almost every class.

			// The three every 2D thing reports and nothing may assign.
			Classes::Computed(guiBase2d, ResolvedField<&Resolved::AbsolutePosition>("AbsolutePosition"));
			Classes::Computed(guiBase2d, ResolvedField<&Resolved::AbsoluteSize>("AbsoluteSize"));
			Classes::Computed(guiBase2d, ResolvedField<&Resolved::AbsoluteRotation>("AbsoluteRotation"));

			// The authored geometry.
			Classes::Property<&Element::Position>(guiObject, "Position");
			Classes::Property<&Element::Size>(guiObject, "Size");
			Classes::Property<&Element::AnchorPoint>(guiObject, "AnchorPoint");
			Classes::Property<&Element::Rotation>(guiObject, "Rotation");
			Classes::Property<&Element::ZIndex>(guiObject, "ZIndex");
			Classes::Property<&Element::LayoutOrder>(guiObject, "LayoutOrder");
			Classes::Property<&Element::Visible>(guiObject, "Visible");
			Classes::Property<&Element::ClipsDescendants>(guiObject, "ClipsDescendants");
			Classes::Property<&Element::Active>(guiObject, "Active");
			Classes::Property<&Element::Selectable>(guiObject, "Selectable");
			Classes::Computed(guiObject, EnumField<&Element::Constraint>("SizeConstraint"));
			Classes::Computed(guiObject, EnumField<&Element::Automatic>("AutomaticSize"));

			// The box it draws for itself.
			Classes::Property<&Background::Color>(guiObject, "BackgroundColor3");
			Classes::Property<&Background::Transparency>(guiObject, "BackgroundTransparency");
			Classes::Property<&Background::BorderColor>(guiObject, "BorderColor3");
			Classes::Property<&Background::BorderSizePixel>(guiObject, "BorderSizePixel");
			Classes::Computed(guiObject, EnumField<&Background::Border>("BorderMode"));

			// Text, declared on all three classes that carry a `Label`.
			//
			// **Three declarations rather than one on a shared base**, because
			// there is no shared base to put them on: `TextButton` derives from
			// `GuiButton`, `TextLabel` from `GuiLabel` and `TextBox` straight
			// from `GuiObject`, which is Roblox's tree. A synthetic
			// "TextThing" base would be a class no script has heard of
			// appearing in `:IsA` and in the manifest, which is a worse trade
			// than a loop over three ids.
			for (const ClassId owner : {textButton, textLabel, textBox}) {
				Classes::Property<&Label::Text>(owner, "Text");
				Classes::Property<&Label::Color>(owner, "TextColor3");
				Classes::Property<&Label::Transparency>(owner, "TextTransparency");
				Classes::Property<&Label::Size>(owner, "TextSize");
				Classes::Property<&Label::Wrapped>(owner, "TextWrapped");
				Classes::Property<&Label::Scaled>(owner, "TextScaled");
				Classes::Property<&Label::StrokeColor>(owner, "TextStrokeColor3");
				Classes::Property<&Label::StrokeTransparency>(owner, "TextStrokeTransparency");
				Classes::Property<&Label::LineHeight>(owner, "LineHeight");
				Classes::Computed(owner, EnumField<&Label::Font>("Font"));
				Classes::Computed(owner, EnumField<&Label::XAlignment>("TextXAlignment"));
				Classes::Computed(owner, EnumField<&Label::YAlignment>("TextYAlignment"));
				Classes::Computed(owner, EnumField<&Label::Truncate>("TextTruncate"));
			}

			// Images, on both classes that carry a `Picture`, for the same
			// reason.
			for (const ClassId owner : {imageButton, imageLabel}) {
				Classes::Property<&Picture::Image>(owner, "Image");
				Classes::Property<&Picture::Color>(owner, "ImageColor3");
				Classes::Property<&Picture::Transparency>(owner, "ImageTransparency");
				Classes::Property<&Picture::SliceCenter>(owner, "SliceCenter");
				Classes::Property<&Picture::SliceScale>(owner, "SliceScale");
				Classes::Property<&Picture::TileSize>(owner, "TileSize");
				Classes::Property<&Picture::RectOffset>(owner, "ImageRectOffset");
				Classes::Property<&Picture::RectSize>(owner, "ImageRectSize");
				Classes::Computed(owner, EnumField<&Picture::Scale>("ScaleType"));
			}

			Classes::Property<&Button::AutoButtonColor>(guiButton, "AutoButtonColor");
			Classes::Property<&Button::Modal>(guiButton, "Modal");
			Classes::Property<&Button::Selected>(guiButton, "Selected");

			Classes::Property<&Scrolling::CanvasSize>(scrollingFrame, "CanvasSize");
			Classes::Property<&Scrolling::CanvasPosition>(scrollingFrame, "CanvasPosition");
			Classes::Property<&Scrolling::BarThickness>(scrollingFrame, "ScrollBarThickness");
			Classes::Property<&Scrolling::BarColor>(scrollingFrame, "ScrollBarImageColor3");
			Classes::Property<&Scrolling::BarTransparency>(scrollingFrame, "ScrollBarImageTransparency");
			Classes::Property<&Scrolling::Enabled>(scrollingFrame, "ScrollingEnabled");
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::Direction>("ScrollingDirection"));
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::AutomaticCanvas>("AutomaticCanvasSize"));

			Classes::Property<&Entry::PlaceholderText>(textBox, "PlaceholderText");
			Classes::Property<&Entry::PlaceholderColor>(textBox, "PlaceholderColor3");
			Classes::Property<&Entry::ClearTextOnFocus>(textBox, "ClearTextOnFocus");
			Classes::Property<&Entry::MultiLine>(textBox, "MultiLine");
			Classes::Property<&Entry::TextEditable>(textBox, "TextEditable");
			Classes::Property<&Entry::CursorPosition>(textBox, "CursorPosition");
			Classes::Property<&Entry::SelectionStart>(textBox, "SelectionStart");

			Classes::Property<&Group::Color>(canvasGroup, "GroupColor3");
			Classes::Property<&Group::Transparency>(canvasGroup, "GroupTransparency");

			Classes::Property<&Viewport::CurrentCamera>(viewportFrame, "CurrentCamera");
			Classes::Property<&Viewport::Ambient>(viewportFrame, "Ambient");
			Classes::Property<&Viewport::LightColor>(viewportFrame, "LightColor");
			Classes::Property<&Viewport::LightDirection>(viewportFrame, "LightDirection");
			Classes::Property<&Viewport::Color>(viewportFrame, "ImageColor3");
			Classes::Property<&Viewport::Transparency>(viewportFrame, "ImageTransparency");

			Classes::Property<&Layer::Enabled>(layerCollector, "Enabled");
			Classes::Property<&Layer::DisplayOrder>(layerCollector, "DisplayOrder");
			Classes::Property<&Layer::ResetOnSpawn>(layerCollector, "ResetOnSpawn");
			Classes::Computed(layerCollector, EnumField<&Layer::Behavior>("ZIndexBehavior"));

			// On `ScreenGui` alone, because the inset is the *screen*'s top bar
			// and a surface gui projected onto a wall has no such thing.
			Classes::Property<&Layer::IgnoreGuiInset>(screenGui, "IgnoreGuiInset");

			Classes::Property<&Surface::Adornee>(surfaceGui, "Adornee");
			Classes::Property<&Surface::PixelsPerStud>(surfaceGui, "PixelsPerStud");
			Classes::Property<&Surface::CanvasSize>(surfaceGui, "CanvasSize");
			Classes::Property<&Surface::AlwaysOnTop>(surfaceGui, "AlwaysOnTop");
			Classes::Property<&Surface::LightInfluence>(surfaceGui, "LightInfluence");
			Classes::Property<&Surface::Brightness>(surfaceGui, "Brightness");
			Classes::Computed(surfaceGui, EnumField<&Surface::On>("Face"));
			Classes::Computed(surfaceGui, EnumField<&Surface::Sizing>("SizingMode"));

			Classes::Property<&Billboard::Adornee>(billboardGui, "Adornee");
			Classes::Property<&Billboard::Size>(billboardGui, "Size");
			Classes::Property<&Billboard::StudsOffset>(billboardGui, "StudsOffset");
			Classes::Property<&Billboard::StudsOffsetWorldSpace>(billboardGui, "StudsOffsetWorldSpace");
			Classes::Property<&Billboard::ExtentsOffset>(billboardGui, "ExtentsOffset");
			Classes::Property<&Billboard::AlwaysOnTop>(billboardGui, "AlwaysOnTop");
			Classes::Property<&Billboard::LightInfluence>(billboardGui, "LightInfluence");
			Classes::Property<&Billboard::MaxDistance>(billboardGui, "MaxDistance");

			Classes::Property<&Padding::Top>(uiPadding, "PaddingTop");
			Classes::Property<&Padding::Bottom>(uiPadding, "PaddingBottom");
			Classes::Property<&Padding::Left>(uiPadding, "PaddingLeft");
			Classes::Property<&Padding::Right>(uiPadding, "PaddingRight");

			Classes::Property<&ListLayout::Padding>(uiListLayout, "Padding");
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Direction>("FillDirection"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Horizontal>("HorizontalAlignment"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Vertical>("VerticalAlignment"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Order>("SortOrder"));

			Classes::Property<&GridLayout::CellSize>(uiGridLayout, "CellSize");
			Classes::Property<&GridLayout::CellPadding>(uiGridLayout, "CellPadding");
			Classes::Property<&GridLayout::MaxCells>(uiGridLayout, "FillDirectionMaxCells");
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Direction>("FillDirection"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Corner>("StartCorner"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Horizontal>("HorizontalAlignment"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Vertical>("VerticalAlignment"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Order>("SortOrder"));

			Classes::Property<&AspectRatio::Ratio>(uiAspect, "AspectRatio");
			Classes::Computed(uiAspect, EnumField<&AspectRatio::Type>("AspectType"));
			Classes::Computed(uiAspect, EnumField<&AspectRatio::Dominant>("DominantAxis"));

			Classes::Property<&SizeLimits::Min>(uiSize, "MinSize");
			Classes::Property<&SizeLimits::Max>(uiSize, "MaxSize");

			Classes::Property<&TextSizeLimits::Min>(uiTextSize, "MinTextSize");
			Classes::Property<&TextSizeLimits::Max>(uiTextSize, "MaxTextSize");

			Classes::Property<&Corner::Radius>(uiCorner, "CornerRadius");

			Classes::Property<&Stroke::Color>(uiStroke, "Color");
			Classes::Property<&Stroke::Thickness>(uiStroke, "Thickness");
			Classes::Property<&Stroke::Transparency>(uiStroke, "Transparency");

			Classes::Property<&Scale::Factor>(uiScale, "Scale");

			Classes::Property<&Adornment::Adornee>(pvAdornment, "Adornee");
			Classes::Property<&Adornment::Color>(pvAdornment, "Color3");
			Classes::Property<&Adornment::Transparency>(pvAdornment, "Transparency");
			Classes::Property<&Adornment::Visible>(pvAdornment, "Visible");
			Classes::Property<&Adornment::AlwaysOnTop>(pvAdornment, "AlwaysOnTop");
			Classes::Property<&Adornment::ZIndex>(pvAdornment, "ZIndex");

			Classes::Property<&SelectionOutline::LineThickness>(selectionBox, "LineThickness");
			Classes::Property<&SelectionOutline::SurfaceColor>(selectionBox, "SurfaceColor3");
			Classes::Property<&SelectionOutline::SurfaceTransparency>(selectionBox, "SurfaceTransparency");

			Classes::Property<&HandleShape::Offset>(handleAdornment, "CFrame");
			Classes::Property<&HandleShape::Size>(handleAdornment, "Size");

			Classes::Property<&GuiServiceState::SelectedObject>(guiService, "SelectedObject");
			Classes::Property<&GuiServiceState::MenuIsOpen>(guiService, "MenuIsOpen");
			Classes::Property<&GuiServiceState::AutoSelectGuiEnabled>(guiService, "AutoSelectGuiEnabled");

			// Referenced so the compiler does not warn about ids the tree needs
			// and no property hangs off. Each is a real class a script may
			// name; none of them adds a property its base has not got.
			(void)canvasGroup;
			(void)dockWidget;
			(void)uiScale;
			(void)selectionSphere;
			(void)boxHandle;
			(void)sphereHandle;
			(void)cylinderHandle;
			(void)lineHandle;
			(void)handles;
			(void)arcHandles;

			return guiObject;
		}
	}

	ecs::ClassId RegisterGuiClasses() {
		static const ClassId object = BuildTree();
		return object;
	}

	ecs::ClassId GuiClass(std::string_view name) {
		return Classes::Find(core::Name(name));
	}

	std::span<const std::string_view> GuiClassNames() {
		return CLASS_NAMES;
	}
}
