#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/RichText.hpp>

#include <array>
#include <cstddef>
#include <string>
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
		// properties panel - which is that loop.
		//
		// **A function template keyed on the C++ enum, not a parameter.** The
		// generated conversions below are captureless function pointers, so
		// they cannot close over a name; keying on the type is what lets one
		// template generate the getter and setter for all twenty-two sets.

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
		GUI_ENUM_NAME(ResampleMode, "ResamplerMode")
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
		GUI_ENUM_NAME(StrokeMode, "ApplyStrokeMode")
		GUI_ENUM_NAME(LineJoin, "LineJoinMode")
		GUI_ENUM_NAME(StrokeSizing, "StrokeSizingMode")
		GUI_ENUM_NAME(core::EasingStyle, "EasingStyle")
		GUI_ENUM_NAME(core::EasingDirection, "EasingDirection")
		GUI_ENUM_NAME(DragStyle, "UIDragDetectorDragStyle")
		GUI_ENUM_NAME(DragResponse, "UIDragDetectorDragStyleResponse")
		GUI_ENUM_NAME(ElasticBehavior, "ElasticBehavior")
		GUI_ENUM_NAME(ScrollBarInset, "ScrollBarInset")
		GUI_ENUM_NAME(BarPosition, "VerticalScrollBarPosition")
		GUI_ENUM_NAME(FlexAlignment, "UIFlexAlignment")
		GUI_ENUM_NAME(ItemLineAlignment, "ItemLineAlignment")
		GUI_ENUM_NAME(FlexMode, "UIFlexMode")
		GUI_ENUM_NAME(ZIndexBehavior, "ZIndexBehavior")
		GUI_ENUM_NAME(SurfaceSizingMode, "SurfaceSizingMode")
		GUI_ENUM_NAME(Face, "NormalId")

#undef GUI_ENUM_NAME

		// The stored value of the *first* member of a set.
		//
		// Zero for every set but one. `ScrollingDirection` is `X = 1`, `Y = 2`,
		// `XY = 3` in Roblox - a bit pair rather than a counter - and the
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
		// header - a count in `Enums.hpp` would be a third place the list lives.
		template <class E> constexpr size_t EnumCount();

#define GUI_ENUM_COUNT(Type, Number)                                                                         \
	template <> constexpr size_t EnumCount<Type>() {                                                         \
		return Number;                                                                                       \
	}

		GUI_ENUM_COUNT(SizeConstraint, 3)
		GUI_ENUM_COUNT(AutomaticSize, 4)
		GUI_ENUM_COUNT(BorderMode, 3)
		GUI_ENUM_COUNT(ScaleType, 5)
		GUI_ENUM_COUNT(ResampleMode, 2)
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
		GUI_ENUM_COUNT(StrokeMode, 2)
		GUI_ENUM_COUNT(LineJoin, 3)
		GUI_ENUM_COUNT(StrokeSizing, 2)
		GUI_ENUM_COUNT(core::EasingStyle, core::EASING_STYLE_COUNT)
		GUI_ENUM_COUNT(core::EasingDirection, core::EASING_DIRECTION_COUNT)
		GUI_ENUM_COUNT(DragStyle, 5)
		GUI_ENUM_COUNT(DragResponse, 4)
		GUI_ENUM_COUNT(ElasticBehavior, 3)
		GUI_ENUM_COUNT(ScrollBarInset, 3)
		GUI_ENUM_COUNT(BarPosition, 2)
		GUI_ENUM_COUNT(FlexAlignment, 5)
		GUI_ENUM_COUNT(ItemLineAlignment, 5)
		GUI_ENUM_COUNT(FlexMode, 5)
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
		// know - so every one of these would have been a property nothing could
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
			} else if constexpr (std::is_same_v<T, bool>) {
				return PropertyType::Bool;
			} else {
				static_assert(std::is_same_v<T, float>, "add a case for this type");
				return PropertyType::Float;
			}
		}

		// A field of a **derived** component, exposed read-only.
		//
		// The component is deduced from the member pointer, so one template
		// serves `Resolved` - what the layout decided - and `SpatialCanvas` -
		// what the host that holds a camera decided. Both are derived every
		// frame from something else, which is what makes read-only the contract
		// rather than a convenience: a write would be gone before the script
		// that made it returned.
		//
		// **A default and not a refusal for a row that has no such component.**
		// `Resolved` is in `GuiBase2d`'s own set and always present, but
		// `SpatialCanvas` is written by whoever holds a camera and a world nobody
		// is drawing has none - and a property that raised `could not read` there
		// would make `CurrentDistance` throw in the studio and answer in the
		// client. `ecs::AuditProperties` refuses that shape outright, and
		// `scene`'s `Mass` is the precedent it names: a derived number nothing
		// has computed yet reads as zero, which is the honest answer.
		template <auto Member> PropertyDescriptor DerivedField(std::string_view name) {
			using Traits = MemberOf<decltype(Member)>;
			using Component = typename Traits::Component;
			using Value = typename Traits::Value;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = TypeOfValue<Value>();
			property.Size = sizeof(Value);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ComponentSet::Intern({Components::Of<Component>()});
			property.Writes = &ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Component *component = store.Get<Component>(instance);
				*static_cast<Value *>(out) = component != nullptr ? component->*Member : Value{};
				return true;
			};

			property.Set = [](ecs::Store &, ecs::Entity, const void *) -> bool { return false; };

			return property;
		}

		// A label's text with its markup stripped, read-only.
		//
		// **Computed on the read rather than stored.** It is a pure function of
		// `Label::Text` and `Label::Rich`, so a second copy in a component would
		// be rule 2's two-statements-of-one-fact - and this is read by a script
		// asking what a person sees, which is a rare call rather than a frame
		// one. `Compile.cpp` parses the same string for its own reasons and the
		// two agree because they call the same parser.
		PropertyDescriptor ContentTextField() {
			PropertyDescriptor property;
			property.Name = core::Name("ContentText");
			property.Type = PropertyType::String;
			property.Size = sizeof(std::string);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			property.Reads = &ComponentSet::Intern({Components::Of<Label>()});
			property.Writes = &ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				auto &result = *static_cast<std::string *>(out);
				const Label *label = store.Get<Label>(instance);
				if (label == nullptr) {
					result.clear();
					return true;
				}
				if (!label->Rich) {
					result = label->Text;
					return true;
				}

				std::vector<DrawSpan> spans;
				ParseRichText(label->Text, *label, result, spans);
				return true;
			};

			property.Set = [](ecs::Store &, ecs::Entity, const void *) -> bool { return false; };
			return property;
		}

		// Every class this module registers, in registration order.
		//
		// **One list, read by three callers** - the registration below, the
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
			"UITableLayout",
			"UIPageLayout",
			"UIConstraint",
			"UIAspectRatioConstraint",
			"UISizeConstraint",
			"UITextSizeConstraint",
			"UIPadding",
			"UICorner",
			"UIStroke",
			"UIScale",
			"UIFlexItem",
			"UIGradient",
			"UIDragDetector",
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
			"ConeHandleAdornment",
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
			RegisterEnum<ResampleMode>();
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
			RegisterEnum<StrokeMode>();
			RegisterEnum<LineJoin>();
			RegisterEnum<StrokeSizing>();

			// **Registered here as well as by the script surface, and that
			// is agreement rather than a clash.** `EnumTable::Register`
			// says so in as many words: two modules may each declare that
			// a set has a member, and refusing the second would make the
			// order two files happened to link in decide whether a build
			// works. Both walk `core::Describe`, so the ordinals - which
			// are the storage - cannot disagree.
			RegisterEnum<core::EasingStyle>();
			RegisterEnum<core::EasingDirection>();
			RegisterEnum<DragStyle>();
			RegisterEnum<DragResponse>();
			RegisterEnum<ElasticBehavior>();
			RegisterEnum<ScrollBarInset>();
			RegisterEnum<BarPosition>();
			RegisterEnum<FlexAlignment>();
			RegisterEnum<ItemLineAlignment>();
			RegisterEnum<FlexMode>();
			RegisterEnum<ZIndexBehavior>();
			RegisterEnum<SurfaceSizingMode>();

			// **The one set this module shares with `scene`.** Both register
			// `NormalId` and `EnumTable` takes the second declaration as
			// agreement - which is legal exactly as long as the orders match,
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
			// `GuiBase`.** It is not a thing that draws - it is the thing that
			// *owns the selection*, which is what finally gives
			// `GuiObject::Selectable` a reader. `scene`'s services sit at the
			// root the same way, and `GetService` finds either by name.
			const std::array guiServiceState{Components::Of<GuiServiceState>()};
			const ClassId guiService = Classes::Register("GuiService", instance, guiServiceState);
			Classes::SetCreatable(guiService, false);

			// --- the 3D branch -----------------------------------------------
			//
			// **Hung off `GuiBase`, which is what that class was kept for.** The
			// comment above said so when the 2D branch went in: `GuiBase` and
			// `GuiBase2d` add no components of their own and a tree that had
			// collapsed the two "would have to grow the split back at exactly
			// the point somebody is adding a feature". This is that point.
			//
			// An adornment is a description rather than a drawing - see
			// `Adornment` - so what is registered here is what to outline and
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

			// A handle is an adornment with a common frame and relative offset.
			// Each leaf owns the dimensions only that shape understands, so a
			// cylinder cannot accidentally expose a box's `Size`.
			const std::array handle{Components::Of<HandleShape>()};
			const ClassId handleAdornment = Classes::Register("HandleAdornment", pvAdornment, handle);

			const std::array boxShape{Components::Of<BoxHandleShape>()};
			const ClassId boxHandle = Classes::Register("BoxHandleAdornment", handleAdornment, boxShape);
			const std::array sphereShape{Components::Of<SphereHandleShape>()};
			const ClassId sphereHandle =
				Classes::Register("SphereHandleAdornment", handleAdornment, sphereShape);
			const std::array cylinderShape{Components::Of<CylinderHandleShape>()};
			const ClassId cylinderHandle =
				Classes::Register("CylinderHandleAdornment", handleAdornment, cylinderShape);
			const std::array lineShape{Components::Of<LineHandleShape>()};
			const ClassId lineHandle = Classes::Register("LineHandleAdornment", handleAdornment, lineShape);
			const std::array coneShape{Components::Of<ConeHandleShape>()};
			const ClassId coneHandle = Classes::Register("ConeHandleAdornment", handleAdornment, coneShape);

			// `Handles` and `ArcHandles` are the draggable ones - the editor's
			// move and rotate gizmos. They carry the same `Adornment` their
			// siblings do and differ in what a drawer offers to grab, which is
			// the drawer's business rather than the tree's.
			const std::array faceHandles{Components::Of<HandlesShape>()};
			const ClassId handles = Classes::Register("Handles", pvAdornment, faceHandles);
			const std::array arcShape{Components::Of<ArcHandlesShape>()};
			const ClassId arcHandles = Classes::Register("ArcHandles", pvAdornment, arcShape);

			// `Resolved` arrives here because both halves below need it: a
			// `LayerCollector` has an absolute rectangle just as an element
			// does, and the layout pass writes both.
			const std::array base2d{Components::Of<Resolved>()};
			const ClassId guiBase2d = Classes::Register("GuiBase2d", guiBase, base2d);

			const std::array object{
				Components::Of<Element>(), Components::Of<Background>(), Components::Of<Selection>()
			};
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
			// can draw a property grid - so there is no window to be its
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

			const std::array tableLayout{Components::Of<TableLayout>()};
			const ClassId uiTableLayout = Classes::Register("UITableLayout", uiLayout, tableLayout);

			const std::array pageLayout{Components::Of<PageLayout>()};
			const ClassId uiPageLayout = Classes::Register("UIPageLayout", uiLayout, pageLayout);

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

			// On the *child* being flexed, not on the layout - Roblox's shape,
			// and the one that lets one spring sit beside fixed buttons.
			const std::array flexItem{Components::Of<FlexItem>()};
			const ClassId uiFlexItem = Classes::Register("UIFlexItem", uiComponent, flexItem);

			// A `UIComponent` rather than a `UIConstraint`, which is Roblox's
			// placement and the right one: a constraint changes where something
			// ends up and this changes what colour it is, so the layout never
			// looks at it and the compile always does.
			const std::array gradient{Components::Of<Gradient>()};
			const ClassId uiGradient = Classes::Register("UIGradient", uiComponent, gradient);

			const std::array dragDetector{Components::Of<DragDetector>()};
			const ClassId uiDragDetector = Classes::Register("UIDragDetector", uiComponent, dragDetector);

			// --- the property surface ----------------------------------------
			//
			// Each declared on the class that first holds what it projects, so
			// a derived class inherits it and `Classes` merges base-first.
			// Declaring them all on the leaves would work today and would be
			// wrong the moment a second subclass exists - which, in this tree,
			// is almost every class.

			// The three every 2D thing reports and nothing may assign.
			Classes::Computed(guiBase2d, DerivedField<&Resolved::AbsolutePosition>("AbsolutePosition"));
			Classes::Computed(guiBase2d, DerivedField<&Resolved::AbsoluteSize>("AbsoluteSize"));
			Classes::Computed(guiBase2d, DerivedField<&Resolved::AbsoluteRotation>("AbsoluteRotation"));

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
			Classes::Property<&Element::Interactable>(guiObject, "Interactable");

			// **The four overrides and the highlight, on `GuiObject` because
			// every one of them is a `GuiObject`'s to answer.** `SelectNext`
			// scores by direction and takes one of these instead whenever it is
			// set, which is Roblox's rule and the one a menu with an awkward
			// layout needs - geometry gets the common case right and an author
			// gets the last word.
			Classes::Property<&Selection::NextUp>(guiObject, "NextSelectionUp");
			Classes::Property<&Selection::NextDown>(guiObject, "NextSelectionDown");
			Classes::Property<&Selection::NextLeft>(guiObject, "NextSelectionLeft");
			Classes::Property<&Selection::NextRight>(guiObject, "NextSelectionRight");
			Classes::Property<&Selection::ImageObject>(guiObject, "SelectionImageObject");
			Classes::Property<&Selection::Order>(guiObject, "SelectionOrder");
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
				Classes::Property<&Label::MaxVisible>(owner, "MaxVisibleGraphemes");
				Classes::Property<&Label::Rich>(owner, "RichText");
				Classes::Computed(owner, ContentTextField());

				// What the layout measured, read-only for `AbsoluteSize`'s
				// reason: a script assigning either would be overwritten by the
				// next pass.
				Classes::Computed(owner, DerivedField<&Resolved::TextBounds>("TextBounds"));
				Classes::Computed(owner, DerivedField<&Resolved::TextFits>("TextFits"));
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
				Classes::Computed(owner, EnumField<&Picture::Resample>("ResampleMode"));

				// **`scene.SurfaceAppearance.Shader`'s exact vocabulary, one
				// indirection flatter** - see `Picture::Shader`'s own header
				// for why an `ImageLabel` names it directly rather than
				// through a child instance.
				Classes::Property<&Picture::Shader>(owner, "Shader");
			}

			// **On `ImageButton` alone**, because an image swapped under the
			// pointer is what a button does with one and an `ImageLabel` has no
			// pointer state to swap on.
			Classes::Property<&Picture::HoverImage>(imageButton, "HoverImage");
			Classes::Property<&Picture::PressedImage>(imageButton, "PressedImage");

			Classes::Property<&Button::AutoButtonColor>(guiButton, "AutoButtonColor");

			Classes::Property<&Scrolling::CanvasSize>(scrollingFrame, "CanvasSize");
			Classes::Property<&Scrolling::CanvasPosition>(scrollingFrame, "CanvasPosition");
			Classes::Property<&Scrolling::BarThickness>(scrollingFrame, "ScrollBarThickness");
			Classes::Property<&Scrolling::BarColor>(scrollingFrame, "ScrollBarImageColor3");
			Classes::Property<&Scrolling::BarTransparency>(scrollingFrame, "ScrollBarImageTransparency");
			Classes::Property<&Scrolling::Enabled>(scrollingFrame, "ScrollingEnabled");
			Classes::Property<&Scrolling::TopImage>(scrollingFrame, "TopImage");
			Classes::Property<&Scrolling::MidImage>(scrollingFrame, "MidImage");
			Classes::Property<&Scrolling::BottomImage>(scrollingFrame, "BottomImage");
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::Direction>("ScrollingDirection"));
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::AutomaticCanvas>("AutomaticCanvasSize"));
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::Elastic>("ElasticBehavior"));
			Classes::Computed(
				scrollingFrame, EnumField<&Scrolling::HorizontalInset>("HorizontalScrollBarInset")
			);
			Classes::Computed(scrollingFrame, EnumField<&Scrolling::VerticalInset>("VerticalScrollBarInset"));
			Classes::Computed(
				scrollingFrame, EnumField<&Scrolling::VerticalBar>("VerticalScrollBarPosition")
			);

			// What the layout worked out, read-only for `Resolved`'s reason: a
			// script assigning either would be overwritten by the next pass.
			Classes::Computed(scrollingFrame, DerivedField<&ScrollState::CanvasSize>("AbsoluteCanvasSize"));
			Classes::Computed(scrollingFrame, DerivedField<&ScrollState::WindowSize>("AbsoluteWindowSize"));

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
			Classes::Property<&Surface::ZOffset>(surfaceGui, "ZOffset");
			Classes::Property<&Surface::MaxDistance>(surfaceGui, "MaxDistance");
			Classes::Property<&Surface::ClipsDescendants>(surfaceGui, "ClipsDescendants");
			Classes::Property<&Surface::Active>(surfaceGui, "Active");
			Classes::Computed(surfaceGui, EnumField<&Surface::On>("Face"));
			Classes::Computed(surfaceGui, EnumField<&Surface::Sizing>("SizingMode"));

			Classes::Property<&Billboard::Adornee>(billboardGui, "Adornee");
			Classes::Property<&Billboard::PlayerToHideFrom>(billboardGui, "PlayerToHideFrom");
			Classes::Property<&Billboard::Size>(billboardGui, "Size");
			Classes::Property<&Billboard::StudsOffset>(billboardGui, "StudsOffset");
			Classes::Property<&Billboard::StudsOffsetWorldSpace>(billboardGui, "StudsOffsetWorldSpace");
			Classes::Property<&Billboard::ExtentsOffset>(billboardGui, "ExtentsOffset");
			Classes::Property<&Billboard::ExtentsOffsetWorldSpace>(billboardGui, "ExtentsOffsetWorldSpace");
			Classes::Property<&Billboard::SizeOffset>(billboardGui, "SizeOffset");
			Classes::Property<&Billboard::AlwaysOnTop>(billboardGui, "AlwaysOnTop");
			Classes::Property<&Billboard::LightInfluence>(billboardGui, "LightInfluence");
			Classes::Property<&Billboard::Brightness>(billboardGui, "Brightness");
			Classes::Property<&Billboard::MaxDistance>(billboardGui, "MaxDistance");
			Classes::Property<&Billboard::DistanceStep>(billboardGui, "DistanceStep");
			Classes::Property<&Billboard::ClipsDescendants>(billboardGui, "ClipsDescendants");
			Classes::Property<&Billboard::Active>(billboardGui, "Active");

			// Read-only, and from the *derived* component rather than the
			// authored one - which is what makes it worth having at all. A script
			// fading a name tag by range wants the number its size was computed
			// from, and `SpatialCanvas::CurrentDistance` is that number. A world
			// nobody is drawing has no such component and the property answers
			// zero, which is the honest reading of "no camera has measured this".
			Classes::Computed(billboardGui, DerivedField<&SpatialCanvas::CurrentDistance>("CurrentDistance"));

			Classes::Property<&Padding::Top>(uiPadding, "PaddingTop");
			Classes::Property<&Padding::Bottom>(uiPadding, "PaddingBottom");
			Classes::Property<&Padding::Left>(uiPadding, "PaddingLeft");
			Classes::Property<&Padding::Right>(uiPadding, "PaddingRight");

			Classes::Property<&ListLayout::Padding>(uiListLayout, "Padding");
			Classes::Property<&ListLayout::Wraps>(uiListLayout, "Wraps");
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Direction>("FillDirection"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Horizontal>("HorizontalAlignment"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Vertical>("VerticalAlignment"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::Order>("SortOrder"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::HorizontalFlex>("HorizontalFlex"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::VerticalFlex>("VerticalFlex"));
			Classes::Computed(uiListLayout, EnumField<&ListLayout::ItemLine>("ItemLineAlignment"));

			Classes::Property<&GridLayout::CellSize>(uiGridLayout, "CellSize");
			Classes::Property<&GridLayout::CellPadding>(uiGridLayout, "CellPadding");
			Classes::Property<&GridLayout::MaxCells>(uiGridLayout, "FillDirectionMaxCells");
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Direction>("FillDirection"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Corner>("StartCorner"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Horizontal>("HorizontalAlignment"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Vertical>("VerticalAlignment"));
			Classes::Computed(uiGridLayout, EnumField<&GridLayout::Order>("SortOrder"));

			Classes::Property<&TableLayout::Padding>(uiTableLayout, "Padding");
			Classes::Property<&TableLayout::FillEmptySpaceColumns>(uiTableLayout, "FillEmptySpaceColumns");
			Classes::Property<&TableLayout::FillEmptySpaceRows>(uiTableLayout, "FillEmptySpaceRows");
			Classes::Computed(uiTableLayout, EnumField<&TableLayout::Direction>("FillDirection"));
			Classes::Computed(uiTableLayout, EnumField<&TableLayout::Horizontal>("HorizontalAlignment"));
			Classes::Computed(uiTableLayout, EnumField<&TableLayout::Vertical>("VerticalAlignment"));
			Classes::Computed(uiTableLayout, EnumField<&TableLayout::Order>("SortOrder"));

			Classes::Property<&PageLayout::CurrentPage>(uiPageLayout, "CurrentPage");
			Classes::Property<&PageLayout::Padding>(uiPageLayout, "Padding");
			Classes::Property<&PageLayout::Circular>(uiPageLayout, "Circular");
			Classes::Computed(uiPageLayout, EnumField<&PageLayout::Direction>("FillDirection"));
			Classes::Computed(uiPageLayout, EnumField<&PageLayout::Order>("SortOrder"));
			Classes::Property<&PageLayout::Animated>(uiPageLayout, "Animated");
			Classes::Property<&PageLayout::TweenTime>(uiPageLayout, "TweenTime");
			Classes::Computed(uiPageLayout, EnumField<&PageLayout::Easing>("EasingStyle"));
			Classes::Computed(uiPageLayout, EnumField<&PageLayout::EasingWay>("EasingDirection"));

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
			Classes::Property<&Stroke::Enabled>(uiStroke, "Enabled");
			Classes::Computed(uiStroke, EnumField<&Stroke::Apply>("ApplyStrokeMode"));
			Classes::Computed(uiStroke, EnumField<&Stroke::Join>("LineJoinMode"));
			Classes::Computed(uiStroke, EnumField<&Stroke::Sizing>("StrokeSizingMode"));

			Classes::Property<&Scale::Factor>(uiScale, "Scale");

			Classes::Property<&Gradient::Color>(uiGradient, "Color");
			Classes::Property<&Gradient::Transparency>(uiGradient, "Transparency");
			Classes::Property<&Gradient::Offset>(uiGradient, "Offset");
			Classes::Property<&Gradient::Rotation>(uiGradient, "Rotation");
			Classes::Property<&Gradient::Enabled>(uiGradient, "Enabled");

			Classes::Property<&DragDetector::BoundingUI>(uiDragDetector, "BoundingUI");
			Classes::Property<&DragDetector::Axis>(uiDragDetector, "DragAxis");
			Classes::Property<&DragDetector::MinTranslation>(uiDragDetector, "MinDragTranslation");
			Classes::Property<&DragDetector::MaxTranslation>(uiDragDetector, "MaxDragTranslation");
			Classes::Property<&DragDetector::Enabled>(uiDragDetector, "Enabled");
			Classes::Computed(uiDragDetector, EnumField<&DragDetector::Style>("DragStyle"));
			Classes::Computed(uiDragDetector, EnumField<&DragDetector::Response>("ResponseStyle"));

			Classes::Property<&FlexItem::GrowRatio>(uiFlexItem, "GrowRatio");
			Classes::Property<&FlexItem::ShrinkRatio>(uiFlexItem, "ShrinkRatio");
			Classes::Computed(uiFlexItem, EnumField<&FlexItem::Mode>("FlexMode"));
			Classes::Computed(uiFlexItem, EnumField<&FlexItem::ItemLine>("ItemLineAlignment"));

			Classes::Property<&Adornment::Adornee>(pvAdornment, "Adornee");
			Classes::Property<&Adornment::Color>(pvAdornment, "Color3");
			Classes::Property<&Adornment::Transparency>(pvAdornment, "Transparency");
			Classes::Property<&Adornment::Visible>(pvAdornment, "Visible");
			Classes::Property<&Adornment::AlwaysOnTop>(pvAdornment, "AlwaysOnTop");
			Classes::Property<&Adornment::ZIndex>(pvAdornment, "ZIndex");

			Classes::Property<&HandleShape::Offset>(handleAdornment, "CFrame");
			Classes::Property<&HandleShape::SizeRelativeOffset>(handleAdornment, "SizeRelativeOffset");
			Classes::Property<&BoxHandleShape::Size>(boxHandle, "Size");
			Classes::Property<&SphereHandleShape::Radius>(sphereHandle, "Radius");
			Classes::Property<&CylinderHandleShape::Radius>(cylinderHandle, "Radius");
			Classes::Property<&CylinderHandleShape::InnerRadius>(cylinderHandle, "InnerRadius");
			Classes::Property<&CylinderHandleShape::Height>(cylinderHandle, "Height");
			Classes::Property<&CylinderHandleShape::Angle>(cylinderHandle, "Angle");
			Classes::Property<&LineHandleShape::Length>(lineHandle, "Length");
			Classes::Property<&LineHandleShape::Thickness>(lineHandle, "Thickness");
			Classes::Property<&ConeHandleShape::Height>(coneHandle, "Height");
			Classes::Property<&ConeHandleShape::Radius>(coneHandle, "Radius");
			Classes::Property<&ConeHandleShape::Hollow>(coneHandle, "Hollow");
			Classes::Property<&HandlesShape::Faces>(handles, "Faces");
			Classes::Property<&ArcHandlesShape::Axes>(arcHandles, "Axes");

			Classes::Property<&GuiServiceState::SelectedObject>(guiService, "SelectedObject");
			Classes::Property<&GuiServiceState::MenuIsOpen>(guiService, "MenuIsOpen");
			Classes::Property<&GuiServiceState::AutoSelectGuiEnabled>(guiService, "AutoSelectGuiEnabled");

			// Referenced so the compiler does not warn about ids the tree needs
			// and no property hangs off. Each is a real class a script may
			// name; none of them adds a property its base has not got.
			(void)canvasGroup;
			(void)dockWidget;
			(void)uiScale;
			// **The three that were `Reserved until the renderer consumes them`,
			// declared at v0.17 now that it does.** They were absent rather than
			// answering a default on `SoundService.cpp`'s rule - a property with
			// nothing behind it reads as decided - and what was actually behind
			// them was nothing at all: `AdornmentGeometry` had no caller and
			// there was no pass to draw its lines.
			//
			// `SelectionSphere` shares the component and therefore the three
			// properties, which is Roblox's arrangement: both are a
			// `PVAdornment` with an outline and a surface, and only the shape a
			// drawer makes of them differs.
			for (const ClassId klass : {selectionBox, selectionSphere}) {
				Classes::Property<&SelectionOutline::LineThickness>(klass, "LineThickness");
				Classes::Property<&SelectionOutline::SurfaceColor>(klass, "SurfaceColor3");
				Classes::Property<&SelectionOutline::SurfaceTransparency>(klass, "SurfaceTransparency");
			}

			(void)boxHandle;
			(void)sphereHandle;
			(void)cylinderHandle;
			(void)lineHandle;
			(void)coneHandle;
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
