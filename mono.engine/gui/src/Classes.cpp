#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Modal.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/RichText.hpp>
#include <engine/gui/Style.hpp>
#include <engine/gui/VirtualCollection.hpp>

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
		using ecs::ClassKind;
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
		GUI_ENUM_NAME(LocalizedArgumentType, "LocalizedArgumentType")
		GUI_ENUM_NAME(FontFace, "Font")
		GUI_ENUM_NAME(FillDirection, "FillDirection")
		GUI_ENUM_NAME(HorizontalAlignment, "HorizontalAlignment")
		GUI_ENUM_NAME(VerticalAlignment, "VerticalAlignment")
		GUI_ENUM_NAME(SortOrder, "SortOrder")
		GUI_ENUM_NAME(StartCorner, "StartCorner")
		GUI_ENUM_NAME(AspectType, "AspectType")
		GUI_ENUM_NAME(DominantAxis, "DominantAxis")
		GUI_ENUM_NAME(ScrollingDirection, "ScrollingDirection")
		GUI_ENUM_NAME(NodePortDirection, "NodePortDirection")
		GUI_ENUM_NAME(NodeBypassMode, "NodeBypassMode")
		GUI_ENUM_NAME(NodePortEdge, "NodePortEdge")
		GUI_ENUM_NAME(InputPortLayout, "InputPortLayout")
		GUI_ENUM_NAME(NodeGroupLayout, "NodeGroupLayout")
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
		GUI_ENUM_NAME(CollectorScaleMode, "CollectorScaleMode")
		GUI_ENUM_NAME(SurfaceSizingMode, "SurfaceSizingMode")
		GUI_ENUM_NAME(ViewportUpdateMode, "ViewportUpdateMode")
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
		GUI_ENUM_COUNT(LocalizedArgumentType, 3)
		GUI_ENUM_COUNT(FontFace, 4)
		GUI_ENUM_COUNT(FillDirection, 2)
		GUI_ENUM_COUNT(HorizontalAlignment, 3)
		GUI_ENUM_COUNT(VerticalAlignment, 3)
		GUI_ENUM_COUNT(SortOrder, 3)
		GUI_ENUM_COUNT(StartCorner, 4)
		GUI_ENUM_COUNT(AspectType, 2)
		GUI_ENUM_COUNT(DominantAxis, 2)
		GUI_ENUM_COUNT(ScrollingDirection, 3)
		GUI_ENUM_COUNT(NodePortDirection, 2)
		GUI_ENUM_COUNT(NodeBypassMode, 2)
		GUI_ENUM_COUNT(NodePortEdge, 3)
		GUI_ENUM_COUNT(InputPortLayout, 3)
		GUI_ENUM_COUNT(NodeGroupLayout, 3)
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
		GUI_ENUM_COUNT(CollectorScaleMode, 5)
		GUI_ENUM_COUNT(SurfaceSizingMode, 2)
		GUI_ENUM_COUNT(ViewportUpdateMode, 4)
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

		template <size_t Index> PropertyDescriptor LocalizedArgumentCountField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<int32_t *>(out) = arguments->Count;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				const int32_t count = *static_cast<const int32_t *>(value);
				if (count < 0 || count > static_cast<int32_t>(LabelLocalizationArguments::MAXIMUM_ARGUMENTS))
					return false;
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Count = static_cast<uint8_t>(count);
				return true;
			};
			return property;
		}

		template <size_t Index> PropertyDescriptor LocalizedArgumentNameField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<core::Name *>(out) = arguments->Values[Index].Name;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Values[Index].Name = *static_cast<const core::Name *>(value);
				return true;
			};
			return property;
		}

		template <size_t Index> PropertyDescriptor LocalizedArgumentStringField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::String;
			property.Size = sizeof(std::string);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<std::string *>(out) = arguments->Values[Index].String;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				const auto &string = *static_cast<const std::string *>(value);
				if (string.size() > LabelLocalizationArguments::MAXIMUM_STRING_BYTES) return false;
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Values[Index].String = string;
				return true;
			};
			return property;
		}

		template <size_t Index> PropertyDescriptor LocalizedArgumentNumberField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Double;
			property.Size = sizeof(double);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<double *>(out) = arguments->Values[Index].Number;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Values[Index].Number = *static_cast<const double *>(value);
				return true;
			};
			return property;
		}

		template <size_t Index> PropertyDescriptor LocalizedArgumentDateField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Int64;
			property.Size = sizeof(int64_t);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<int64_t *>(out) = arguments->Values[Index].UnixSeconds;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Values[Index].UnixSeconds = *static_cast<const int64_t *>(value);
				return true;
			};
			return property;
		}

		template <size_t Index> PropertyDescriptor LocalizedArgumentTypeField(std::string_view name) {
			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = PropertyType::Enum;
			property.EnumName = EnumNameOf<LocalizedArgumentType>();
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ComponentSet::Intern({Components::Of<LabelLocalizationArguments>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) {
				const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				*static_cast<core::Name *>(out) = ecs::EnumTable::MemberAt(
					EnumNameOf<LocalizedArgumentType>(), static_cast<size_t>(arguments->Values[Index].Type)
				);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) {
				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						EnumNameOf<LocalizedArgumentType>(), *static_cast<const core::Name *>(value), ordinal
					) ||
					ordinal >= 3)
					return false;
				auto *arguments = store.GetMutable<LabelLocalizationArguments>(instance);
				if (arguments == nullptr) return false;
				arguments->Values[Index].Type = static_cast<LocalizedArgumentType>(ordinal);
				return true;
			};
			return property;
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
			} else if constexpr (std::is_same_v<T, core::Color3>) {
				return PropertyType::Color3;
			} else if constexpr (std::is_same_v<T, std::string>) {
				return PropertyType::String;
			} else if constexpr (std::is_same_v<T, bool>) {
				return PropertyType::Bool;
			} else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>) {
				return PropertyType::Int32;
			} else {
				static_assert(std::is_same_v<T, float>, "add a case for this type");
				return PropertyType::Float;
			}
		}

		// A direct visual assignment needs one bit of authored provenance in
		// addition to its value. Otherwise setting an engine-default colour is
		// indistinguishable from never touching the field and a theme could
		// overwrite an intentional reset after save or replication.
		template <auto Member, StyleDirectProperty Direct>
		PropertyDescriptor StyledField(std::string_view name) {
			using Traits = MemberOf<decltype(Member)>;
			using Component = typename Traits::Component;
			using Value = typename Traits::Value;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = TypeOfValue<Value>();
			property.Size = sizeof(Value);
			property.Kind = PropertyKind::Field;
			property.Reads =
				&ComponentSet::Intern({Components::Of<Component>(), Components::Of<StyleDirect>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Component *component = store.Get<Component>(instance);
				if (component == nullptr) {
					return false;
				}
				*static_cast<Value *>(out) = component->*Member;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Component *component = store.GetMutable<Component>(instance);
				StyleDirect *direct = store.GetMutable<StyleDirect>(instance);
				if (component == nullptr || direct == nullptr) {
					return false;
				}
				component->*Member = *static_cast<const Value *>(value);
				direct->Set(Direct);
				return true;
			};
			return property;
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
			"NodeCanvas",
			"NodeCanvasNode",
			"NodeCanvasGroup",
			"NodeCanvasPort",
			"NodeCanvasLink",
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
			"UIMask",
			"UIBinding",
			"UIModalScope",
			"UIAnimation",
			"UIVirtualCollection",
			"UITheme",
			"UIStyle",
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
			RegisterEnum<LocalizedArgumentType>();
			RegisterEnum<FontFace>();
			RegisterEnum<FillDirection>();
			RegisterEnum<HorizontalAlignment>();
			RegisterEnum<VerticalAlignment>();
			RegisterEnum<SortOrder>();
			RegisterEnum<StartCorner>();
			RegisterEnum<AspectType>();
			RegisterEnum<DominantAxis>();
			RegisterEnum<ScrollingDirection>();
			RegisterEnum<NodePortDirection>();
			RegisterEnum<NodeBypassMode>();
			RegisterEnum<NodePortEdge>();
			RegisterEnum<InputPortLayout>();
			RegisterEnum<NodeGroupLayout>();
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
			RegisterEnum<CollectorScaleMode>();
			RegisterEnum<SurfaceSizingMode>();
			RegisterEnum<ViewportUpdateMode>();

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
			Classes::SetCreatable(guiBase, false);
			Classes::SetKind(guiBase, ClassKind::Abstract);
			Classes::SetStudioVisible(guiBase, false);

			// **A service, so it hangs off `Instance` rather than off
			// `GuiBase`.** It is not a thing that draws - it is the thing that
			// *owns the selection*, which is what finally gives
			// `GuiObject::Selectable` a reader. `scene`'s services sit at the
			// root the same way, and `GetService` finds either by name.
			const std::array guiServiceState{
				Components::Of<GuiServiceState>(),
				Components::Of<VirtualFocusState>(),
				Components::Of<TextCompositionState>()
			};
			const ClassId guiService = Classes::Register("GuiService", instance, guiServiceState);
			Classes::SetCreatable(guiService, false);
			Classes::SetKind(guiService, ClassKind::Service);
			Classes::SetStudioVisible(guiService, false);

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
			Classes::SetCreatable(guiBase3d, false);
			Classes::SetKind(guiBase3d, ClassKind::Abstract);
			Classes::SetStudioVisible(guiBase3d, false);

			// `PVAdornment` is Roblox's name for "an adornment about a
			// `BasePart`", and the `Adornee` lives here rather than on
			// `GuiBase3d` because that is where Roblox puts it.
			const std::array adornment{Components::Of<Adornment>(), Components::Of<AdornmentInteraction>()};
			const ClassId pvAdornment = Classes::Register("PVAdornment", guiBase3d, adornment);
			Classes::SetCreatable(pvAdornment, false);
			Classes::SetKind(pvAdornment, ClassKind::Abstract);
			Classes::SetStudioVisible(pvAdornment, false);

			const std::array outline{Components::Of<SelectionOutline>()};
			const ClassId selectionBox = Classes::Register("SelectionBox", pvAdornment, outline);
			const ClassId selectionSphere = Classes::Register("SelectionSphere", pvAdornment, outline);

			// A handle is an adornment with a common frame and relative offset.
			// Each leaf owns the dimensions only that shape understands, so a
			// cylinder cannot accidentally expose a box's `Size`.
			const std::array handle{Components::Of<HandleShape>()};
			const ClassId handleAdornment = Classes::Register("HandleAdornment", pvAdornment, handle);
			Classes::SetCreatable(handleAdornment, false);
			Classes::SetKind(handleAdornment, ClassKind::Abstract);
			Classes::SetStudioVisible(handleAdornment, false);

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
			Classes::SetCreatable(guiBase2d, false);
			Classes::SetKind(guiBase2d, ClassKind::Abstract);
			Classes::SetStudioVisible(guiBase2d, false);

			const std::array object{
				Components::Of<Element>(),
				Components::Of<Background>(),
				Components::Of<Selection>(),
				Components::Of<StyleClass>(),
				Components::Of<StyleDirect>()
			};
			const ClassId guiObject = Classes::Register("GuiObject", guiBase2d, object);
			Classes::SetCreatable(guiObject, false);
			Classes::SetKind(guiObject, ClassKind::Abstract);
			Classes::SetStudioVisible(guiObject, false);

			const ClassId frame = Classes::Register("Frame", guiObject, {});

			const std::array group{Components::Of<Group>()};
			const ClassId canvasGroup = Classes::Register("CanvasGroup", frame, group);

			const std::array scrolling{Components::Of<Scrolling>()};
			const ClassId scrollingFrame = Classes::Register("ScrollingFrame", frame, scrolling);

			const std::array nodeCanvas{Components::Of<NodeCanvas>()};
			const ClassId nodeCanvasClass = Classes::Register("NodeCanvas", frame, nodeCanvas);
			const std::array node{Components::Of<NodeCanvasNode>()};
			const ClassId nodeCanvasNode = Classes::Register("NodeCanvasNode", frame, node);
			const std::array nodeGroup{Components::Of<NodeCanvasGroup>()};
			const ClassId nodeCanvasGroup = Classes::Register("NodeCanvasGroup", frame, nodeGroup);
			const std::array port{Components::Of<NodeCanvasPort>()};
			const ClassId nodeCanvasPort = Classes::Register("NodeCanvasPort", guiObject, port);
			const std::array link{Components::Of<NodeCanvasLink>()};
			const ClassId nodeCanvasLink = Classes::Register("NodeCanvasLink", instance, link);

			const std::array button{Components::Of<Button>()};
			const ClassId guiButton = Classes::Register("GuiButton", guiObject, button);
			Classes::SetCreatable(guiButton, false);
			Classes::SetKind(guiButton, ClassKind::Abstract);
			Classes::SetStudioVisible(guiButton, false);

			const std::array label{
				Components::Of<Label>(),
				Components::Of<LabelPresentation>(),
				Components::Of<LabelLocalizationArguments>()
			};
			const std::array picture{Components::Of<Picture>()};

			const ClassId textButton = Classes::Register("TextButton", guiButton, label);
			const ClassId imageButton = Classes::Register("ImageButton", guiButton, picture);

			const ClassId guiLabel = Classes::Register("GuiLabel", guiObject, {});
			Classes::SetCreatable(guiLabel, false);
			Classes::SetKind(guiLabel, ClassKind::Abstract);
			Classes::SetStudioVisible(guiLabel, false);
			const ClassId textLabel = Classes::Register("TextLabel", guiLabel, label);
			const ClassId imageLabel = Classes::Register("ImageLabel", guiLabel, picture);

			// A text box is a label you can type into, so it carries both.
			const std::array entry{
				Components::Of<Label>(),
				Components::Of<LabelPresentation>(),
				Components::Of<LabelLocalizationArguments>(),
				Components::Of<Entry>()
			};
			const ClassId textBox = Classes::Register("TextBox", guiObject, entry);

			const std::array viewport{Components::Of<Viewport>()};
			const ClassId viewportFrame = Classes::Register("ViewportFrame", guiObject, viewport);

			// --- the collectors ----------------------------------------------

			const std::array collector{
				Components::Of<Layer>(), Components::Of<Canvas>(), Components::Of<ThemeBinding>()
			};
			const ClassId layerCollector = Classes::Register("LayerCollector", guiBase2d, collector);
			Classes::SetCreatable(layerCollector, false);
			Classes::SetKind(layerCollector, ClassKind::Abstract);
			Classes::SetStudioVisible(layerCollector, false);

			const ClassId screenGui = Classes::Register("ScreenGui", layerCollector, {});

			const std::array surface{Components::Of<Surface>()};
			const ClassId surfaceGui = Classes::Register("SurfaceGui", layerCollector, surface);

			const std::array billboard{Components::Of<Billboard>()};
			const ClassId billboardGui = Classes::Register("BillboardGui", layerCollector, billboard);

			// A `PluginGui` gets its canvas from a host window rather than from
			// world containment. Studio supplies that rectangle through
			// `LayoutCollector`; ordinary screen and spatial layout deliberately
			// skip it because neither owns the dock's content area.
			const ClassId pluginGui = Classes::Register("PluginGui", layerCollector, {});
			Classes::SetCreatable(pluginGui, false);
			Classes::SetKind(pluginGui, ClassKind::Abstract);
			Classes::SetStudioVisible(pluginGui, false);
			const ClassId dockWidget = Classes::Register("DockWidgetPluginGui", pluginGui, {});

			// --- the modifiers -----------------------------------------------

			const ClassId uiBase = Classes::Register("UIBase", instance, {});
			const ClassId uiComponent = Classes::Register("UIComponent", uiBase, {});
			const ClassId uiLayout = Classes::Register("UILayout", uiComponent, {});
			Classes::SetCreatable(uiBase, false);
			Classes::SetCreatable(uiComponent, false);
			Classes::SetCreatable(uiLayout, false);
			Classes::SetKind(uiBase, ClassKind::Abstract);
			Classes::SetKind(uiComponent, ClassKind::Abstract);
			Classes::SetKind(uiLayout, ClassKind::Abstract);
			Classes::SetStudioVisible(uiBase, false);
			Classes::SetStudioVisible(uiComponent, false);
			Classes::SetStudioVisible(uiLayout, false);

			const std::array listLayout{Components::Of<ListLayout>()};
			const ClassId uiListLayout = Classes::Register("UIListLayout", uiLayout, listLayout);

			const std::array gridLayout{Components::Of<GridLayout>()};
			const ClassId uiGridLayout = Classes::Register("UIGridLayout", uiLayout, gridLayout);

			const std::array tableLayout{Components::Of<TableLayout>()};
			const ClassId uiTableLayout = Classes::Register("UITableLayout", uiLayout, tableLayout);

			const std::array pageLayout{Components::Of<PageLayout>()};
			const ClassId uiPageLayout = Classes::Register("UIPageLayout", uiLayout, pageLayout);

			const ClassId uiConstraint = Classes::Register("UIConstraint", uiComponent, {});
			Classes::SetCreatable(uiConstraint, false);
			Classes::SetKind(uiConstraint, ClassKind::Abstract);
			Classes::SetStudioVisible(uiConstraint, false);

			const std::array mask{Components::Of<Mask>()};
			const ClassId uiMask = Classes::Register("UIMask", uiComponent, mask);

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

			const std::array binding{
				Components::Of<Binding>(),
				Components::Of<BindingOutput>(),
				Components::Of<BindingDependency>()
			};
			const ClassId uiBinding = Classes::Register("UIBinding", instance, binding);
			const std::array modalScope{Components::Of<ModalScope>()};
			const ClassId uiModalScope = Classes::Register("UIModalScope", uiComponent, modalScope);
			const std::array animation{Components::Of<AnimationPlayback>()};
			const ClassId uiAnimation = Classes::Register("UIAnimation", uiComponent, animation);
			const std::array style{Components::Of<UIStyle>()};
			Classes::Register("UIStyle", uiComponent, style);
			const std::array theme{Components::Of<UITheme>()};
			Classes::Register("UITheme", instance, theme);

			const std::array virtualCollection{Components::Of<VirtualCollection>()};
			const ClassId uiVirtualCollection =
				Classes::Register("UIVirtualCollection", instance, virtualCollection);

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
			Classes::Property<&ThemeBinding::Theme>(layerCollector, "Theme");

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
			Classes::Computed(
				guiObject,
				StyledField<&Background::Color, StyleDirectProperty::BackgroundColor>("BackgroundColor3")
			);
			Classes::Computed(
				guiObject,
				StyledField<&Background::Transparency, StyleDirectProperty::BackgroundTransparency>(
					"BackgroundTransparency"
				)
			);
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
				Classes::Property<&LabelPresentation::LocalizationKey>(owner, "LocalizationKey");
				Classes::Computed(owner, LocalizedArgumentCountField<0>("LocalizationArgumentCount"));
				Classes::Computed(
					owner, StyledField<&Label::Color, StyleDirectProperty::TextColor>("TextColor3")
				);
				Classes::Computed(
					owner,
					StyledField<&Label::Transparency, StyleDirectProperty::TextTransparency>(
						"TextTransparency"
					)
				);
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
				Classes::Computed(
					owner, StyledField<&Picture::Color, StyleDirectProperty::ImageColor>("ImageColor3")
				);
				Classes::Computed(
					owner,
					StyledField<&Picture::Transparency, StyleDirectProperty::ImageTransparency>(
						"ImageTransparency"
					)
				);
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

			Classes::Property<&NodeCanvas::Pan>(nodeCanvasClass, "CanvasPosition");
			Classes::Property<&NodeCanvas::Zoom>(nodeCanvasClass, "Zoom");
			Classes::Property<&NodeCanvas::MinimumZoom>(nodeCanvasClass, "MinimumZoom");
			Classes::Property<&NodeCanvas::MaximumZoom>(nodeCanvasClass, "MaximumZoom");
			Classes::Property<&NodeCanvas::GridSize>(nodeCanvasClass, "GridSize");
			Classes::Property<&NodeCanvas::GridVisible>(nodeCanvasClass, "GridVisible");

			Classes::Property<&NodeCanvasNode::Id>(nodeCanvasNode, "NodeId");
			Classes::Property<&NodeCanvasNode::Type>(nodeCanvasNode, "NodeType");
			Classes::Property<&NodeCanvasNode::Title>(nodeCanvasNode, "Title");
			Classes::Property<&NodeCanvasNode::Enabled>(nodeCanvasNode, "Enabled");
			Classes::Computed(nodeCanvasNode, EnumField<&NodeCanvasNode::BypassMode>("BypassMode"));
			Classes::Property<&NodeCanvasNode::BypassInput>(nodeCanvasNode, "BypassInput");
			Classes::Property<&NodeCanvasNode::BypassOutput>(nodeCanvasNode, "BypassOutput");
			Classes::Property<&NodeCanvasNode::MinimumSize>(nodeCanvasNode, "MinimumSize");
			Classes::Property<&NodeCanvasNode::Resizable>(nodeCanvasNode, "Resizable");
			Classes::Computed(nodeCanvasNode, EnumField<&NodeCanvasNode::InputLayout>("InputPortLayout"));

			Classes::Property<&NodeCanvasGroup::Id>(nodeCanvasGroup, "GroupId");
			Classes::Property<&NodeCanvasGroup::Title>(nodeCanvasGroup, "Title");
			Classes::Property<&NodeCanvasGroup::Padding>(nodeCanvasGroup, "Padding");
			Classes::Computed(nodeCanvasGroup, EnumField<&NodeCanvasGroup::Layout>("Layout"));

			Classes::Property<&NodeCanvasPort::Id>(nodeCanvasPort, "PortId");
			Classes::Property<&NodeCanvasPort::ValueType>(nodeCanvasPort, "ValueType");
			Classes::Computed(nodeCanvasPort, EnumField<&NodeCanvasPort::Direction>("Direction"));
			Classes::Computed(nodeCanvasPort, EnumField<&NodeCanvasPort::Edge>("Edge"));
			Classes::Property<&NodeCanvasPort::MaxConnections>(nodeCanvasPort, "MaxConnections");

			Classes::Property<&NodeCanvasLink::FromNode>(nodeCanvasLink, "FromNode");
			Classes::Property<&NodeCanvasLink::FromPort>(nodeCanvasLink, "FromPort");
			Classes::Computed(nodeCanvasLink, EnumField<&NodeCanvasLink::FromDirection>("FromDirection"));
			Classes::Property<&NodeCanvasLink::ToNode>(nodeCanvasLink, "ToNode");
			Classes::Property<&NodeCanvasLink::ToPort>(nodeCanvasLink, "ToPort");
			Classes::Computed(nodeCanvasLink, EnumField<&NodeCanvasLink::ToDirection>("ToDirection"));
			Classes::Property<&NodeCanvasLink::LineColor>(nodeCanvasLink, "LineColor3");
			Classes::Property<&NodeCanvasLink::LineTransparency>(nodeCanvasLink, "LineTransparency");
			Classes::Property<&NodeCanvasLink::LineThickness>(nodeCanvasLink, "LineThickness");

			Classes::Property<&Entry::PlaceholderText>(textBox, "PlaceholderText");
			Classes::Property<&Entry::PlaceholderColor>(textBox, "PlaceholderColor3");
			Classes::Property<&Entry::ClearTextOnFocus>(textBox, "ClearTextOnFocus");
			Classes::Property<&Entry::MultiLine>(textBox, "MultiLine");
			Classes::Property<&Entry::TextEditable>(textBox, "TextEditable");
			Classes::Property<&Entry::Password>(textBox, "Password");
			Classes::Property<&Entry::CursorPosition>(textBox, "CursorPosition");
			Classes::Property<&Entry::SelectionStart>(textBox, "SelectionStart");

			Classes::Property<&Group::Color>(canvasGroup, "GroupColor3");
			Classes::Property<&Group::Transparency>(canvasGroup, "GroupTransparency");
			Classes::Property<&Mask::Radius>(uiMask, "CornerRadius");
			Classes::Property<&Mask::Enabled>(uiMask, "Enabled");

			Classes::Property<&Viewport::CurrentCamera>(viewportFrame, "CurrentCamera");
			Classes::Property<&Viewport::Ambient>(viewportFrame, "Ambient");
			Classes::Property<&Viewport::LightColor>(viewportFrame, "LightColor");
			Classes::Property<&Viewport::LightDirection>(viewportFrame, "LightDirection");
			Classes::Property<&Viewport::Color>(viewportFrame, "ImageColor3");
			Classes::Property<&Viewport::Transparency>(viewportFrame, "ImageTransparency");
			Classes::Property<&Viewport::ResolutionScale>(viewportFrame, "ResolutionScale");
			Classes::Computed(viewportFrame, EnumField<&Viewport::UpdateMode>("UpdateMode"));
			Classes::Property<&Viewport::UpdateEveryFrames>(viewportFrame, "UpdateEveryFrames");
			Classes::Property<&Viewport::InvalidationRevision>(viewportFrame, "InvalidationRevision");

			Classes::Property<&Layer::Enabled>(layerCollector, "Enabled");
			Classes::Property<&Layer::DisplayOrder>(layerCollector, "DisplayOrder");
			Classes::Property<&Layer::ResetOnSpawn>(layerCollector, "ResetOnSpawn");
			Classes::Computed(layerCollector, EnumField<&Layer::Behavior>("ZIndexBehavior"));
			Classes::Property<&Layer::ReferenceResolution>(layerCollector, "ReferenceResolution");
			Classes::Computed(layerCollector, EnumField<&Layer::ScaleMode>("ScaleMode"));

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

			Classes::Property<&Binding::SourcePath>(uiBinding, "SourcePath");
			Classes::Property<&Binding::Attribute>(uiBinding, "Attribute");
			Classes::Property<&Binding::Target>(uiBinding, "Target");
			Classes::Property<&Binding::Fallback>(uiBinding, "Fallback");
			Classes::Computed(uiBinding, DerivedField<&BindingOutput::Value>("Value"));
			Classes::Computed(uiBinding, DerivedField<&BindingOutput::Valid>("Valid"));
			Classes::Property<&ModalScope::Enabled>(uiModalScope, "Enabled");
			Classes::Property<&AnimationPlayback::StartedAt>(uiAnimation, "StartTime");
			Classes::Property<&AnimationPlayback::Playing>(uiAnimation, "Playing");

			Classes::Property<&VirtualCollection::ItemCount>(uiVirtualCollection, "ItemCount");
			Classes::Property<&VirtualCollection::FixedExtent>(uiVirtualCollection, "FixedExtent");
			Classes::Property<&VirtualCollection::Revision>(uiVirtualCollection, "Revision");

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
			Classes::Property<&AdornmentInteraction::Enabled>(pvAdornment, "InteractionEnabled");

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
			(void)uiBinding;
			(void)uiModalScope;
			(void)uiVirtualCollection;
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
