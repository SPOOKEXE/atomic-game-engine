#pragma once

// Typed visual values and the deliberately small GUI style cascade.
//
// A style is a list of named values rather than a selector language. The caller
// supplies the classes and interaction state it has already resolved, then this
// file applies theme values, matching class rules, local rules, and direct
// values in that order. No tree walk, script call, or property mutation hides
// behind resolving a style.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::gui {

	// The payload kind held by a style value.
	enum class StyleValueType : uint8_t {
		Color,
		Number,
	};

	// Interaction facts a host has already derived. A rule names the facts it
	// needs and never queries input or accessibility state by itself.
	enum class StyleState : uint8_t {
		None = 0,
		Hovered = 1 << 0,
		Pressed = 1 << 1,
		Focused = 1 << 2,
		Selected = 1 << 3,
		Disabled = 1 << 4,
		Invalid = 1 << 5,
	};

	// Combines two interaction-state masks.
	constexpr StyleState operator|(StyleState left, StyleState right) {
		return static_cast<StyleState>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
	}

	// Whether state contains every required interaction flag.
	constexpr bool Includes(StyleState state, StyleState required) {
		return (static_cast<uint8_t>(state) & static_cast<uint8_t>(required)) ==
			   static_cast<uint8_t>(required);
	}

	// One value with an explicit type tag. The unused member is never read.
	struct StyleValue {
		// Selects the active style payload.
		StyleValueType Type = StyleValueType::Color;
		// Colour payload when Type is Color.
		core::Color3 Color{};
		// Scalar payload when Type is Number.
		float Number = 0.0f;

		// Creates a colour style value.
		static StyleValue FromColor(core::Color3 value) {
			StyleValue result;
			result.Color = value;
			return result;
		}

		// Creates a numeric style value.
		static StyleValue FromNumber(float value) {
			StyleValue result;
			result.Type = StyleValueType::Number;
			result.Number = value;
			return result;
		}
	};

	// One named typed style token.
	struct StyleDeclaration {
		// Stable token name.
		core::Name Name;
		// Authored token value.
		StyleValue Value;
	};

	// A bounded typed dictionary. Refusing a replacement with another type is
	// what prevents a colour token from becoming a number by accident.
	class StyleSet {
	  public:
		// Largest number of tokens in this set.
		static constexpr size_t MAXIMUM_DECLARATIONS = 32;

		// Adds or replaces a token of the same type.
		bool Set(StyleDeclaration declaration);
		// Finds a token by stable name.
		const StyleValue *Find(core::Name name) const;
		// Tokens in authored insertion order.
		std::span<const StyleDeclaration> Declarations() const;
		// Monotonic revision of accepted changes.
		uint64_t Revision() const {
			return RevisionNumber;
		}

	  private:
		std::array<StyleDeclaration, MAXIMUM_DECLARATIONS> Values{};
		size_t Count = 0;
		uint64_t RevisionNumber = 0;
	};

	// The stable class names attached to an element. Their order is significant:
	// later classes win over earlier ones, which makes an authored override
	// inspectable without selector specificity arithmetic.
	class StyleClasses {
	  public:
		// Largest number of attached stable class names.
		static constexpr size_t MAXIMUM_CLASSES = 8;

		// Adds a class name within fixed capacity.
		bool Add(core::Name name);
		// Attached class names in cascade order.
		std::span<const core::Name> Names() const;

	  private:
		std::array<core::Name, MAXIMUM_CLASSES> Values{};
		size_t Count = 0;
	};

	// Authored token table on a named UITheme instance. Theme selection belongs
	// to the collector, so the same retained tree can compile for two viewers
	// without copying style values into either adapter.
	struct UITheme {
		// Theme token table.
		StyleSet Tokens;
	};

	// Stable class names on a GuiObject. The fixed capacity is part of the file
	// and wire contract: a hostile document cannot turn class matching into an
	// unbounded selector loop.
	struct StyleClass {
		// Stable class names attached to the target.
		StyleClasses Names;
	};

	// Records that a visual property was assigned through the shared property
	// surface, even when the assigned value equals its engine default. The mask
	// is authored state, so save and replication preserve direct precedence.
	enum class StyleDirectProperty : uint8_t {
		BackgroundColor = 1 << 0,
		BackgroundTransparency = 1 << 1,
		TextColor = 1 << 2,
		TextTransparency = 1 << 3,
		ImageColor = 1 << 4,
		ImageTransparency = 1 << 5,
	};

	// Authored direct-property precedence mask.
	struct StyleDirect {
		// Bit mask of directly assigned visual properties.
		uint8_t Properties = 0;

		// Whether a property was assigned directly.
		bool Has(StyleDirectProperty property) const {
			return (Properties & static_cast<uint8_t>(property)) != 0;
		}

		// Marks a property as directly assigned.
		void Set(StyleDirectProperty property) {
			Properties |= static_cast<uint8_t>(property);
		}
	};

	// A declaration group. An empty class is local to the target. A non-empty
	// class matches only that exact stable class name.
	struct StyleRule {
		// Optional class this rule matches.
		core::Name Class;
		// Interaction flags required to match.
		StyleState State = StyleState::None;
		// Tokens supplied by this rule.
		StyleSet Declarations;
	};

	// An authored UIStyle child. Class is optional for an element-local rule and
	// states are matched only against interaction facts supplied by compile.
	struct UIStyle {
		// Rule authored by this child instance.
		StyleRule Rule;
	};

	// The collector's explicit reference to a UITheme instance. An entity handle
	// is saved through Store's normal remapping and is never a process pointer.
	struct ThemeBinding {
		// Theme resource selected by the collector.
		ecs::Entity Theme;
	};

	// Viewer-local compile output. It is never authored or replicated; scripts
	// continue to read the direct properties they wrote.
	struct ResolvedStyle {
		// Final resolved token table.
		StyleSet Values;
		// Interaction facts used for resolution.
		StyleState State = StyleState::None;
	};

	// Largest number of style rules collected for one target.
	constexpr size_t MAXIMUM_STYLE_RULES = 64;

	// The layer that supplied a resolved token. This stays alongside compile
	// output so an authoring tool can explain a value without recreating the
	// cascade with subtly different precedence.
	enum class StyleSource : uint8_t {
		Theme,
		ClassRule,
		LocalRule,
		Direct,
	};

	// Source information for one resolved style token.
	struct StyleProvenance {
		// Marker for values that do not originate from a rule.
		static constexpr size_t NO_RULE = static_cast<size_t>(-1);

		// Resolved token name.
		core::Name Name;
		// Resolved token value.
		StyleValue Value;
		// Cascade layer that supplied Value.
		StyleSource Source = StyleSource::Theme;
		// Matching class for a class rule.
		core::Name Class;
		// Matching interaction state for a rule.
		StyleState State = StyleState::None;
		// Index in the ResolveStyleTrace rules span, or NO_RULE for theme and
		// direct values. A host can map this back to the authored UIStyle child.
		size_t RuleIndex = NO_RULE;
	};

	// Resolved tokens with their winning cascade layers.
	struct StyleResolution {
		// Final resolved token table.
		StyleSet Values;
		// Provenance storage parallel to Values.
		std::array<StyleProvenance, StyleSet::MAXIMUM_DECLARATIONS> Provenance{};
		// Number of active provenance entries.
		size_t Count = 0;

		// Active provenance entries in token order.
		std::span<const StyleProvenance> Sources() const {
			return {Provenance.data(), Count};
		}
	};

	// Resolves one element's visual values. `theme` stands for the collector
	// theme, `rules` are matching style-class and local declarations in authored
	// order, and `direct` contains values explicitly written on the instance.
	//
	// @return `false` when a bounded set fills or a layer tries to change an
	//         existing token's type. `out` then contains no partial result.
	bool ResolveStyle(
		const StyleSet &theme,
		const StyleClasses &classes,
		std::span<const StyleRule> rules,
		const StyleSet &direct,
		StyleState state,
		StyleSet &out
	);

	// Resolves the same cascade as ResolveStyle and records the winning layer
	// for every token. `out` contains no partial result when resolution fails.
	bool ResolveStyleTrace(
		const StyleSet &theme,
		const StyleClasses &classes,
		std::span<const StyleRule> rules,
		const StyleSet &direct,
		StyleState state,
		StyleResolution &out
	);
}
