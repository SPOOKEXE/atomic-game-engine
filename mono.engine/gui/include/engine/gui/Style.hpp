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

	constexpr StyleState operator|(StyleState left, StyleState right) {
		return static_cast<StyleState>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
	}

	constexpr bool Includes(StyleState state, StyleState required) {
		return (static_cast<uint8_t>(state) & static_cast<uint8_t>(required)) ==
			   static_cast<uint8_t>(required);
	}

	// One value with an explicit type tag. The unused member is never read.
	struct StyleValue {
		StyleValueType Type = StyleValueType::Color;
		core::Color3 Color{};
		float Number = 0.0f;

		static StyleValue FromColor(core::Color3 value) {
			StyleValue result;
			result.Color = value;
			return result;
		}

		static StyleValue FromNumber(float value) {
			StyleValue result;
			result.Type = StyleValueType::Number;
			result.Number = value;
			return result;
		}
	};

	struct StyleDeclaration {
		core::Name Name;
		StyleValue Value;
	};

	// A bounded typed dictionary. Refusing a replacement with another type is
	// what prevents a colour token from becoming a number by accident.
	class StyleSet {
	  public:
		static constexpr size_t MAXIMUM_DECLARATIONS = 32;

		bool Set(StyleDeclaration declaration);
		const StyleValue *Find(core::Name name) const;
		std::span<const StyleDeclaration> Declarations() const;
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
		static constexpr size_t MAXIMUM_CLASSES = 8;

		bool Add(core::Name name);
		std::span<const core::Name> Names() const;

	  private:
		std::array<core::Name, MAXIMUM_CLASSES> Values{};
		size_t Count = 0;
	};

	// Authored token table on a named UITheme instance. Theme selection belongs
	// to the collector, so the same retained tree can compile for two viewers
	// without copying style values into either adapter.
	struct UITheme {
		StyleSet Tokens;
	};

	// Stable class names on a GuiObject. The fixed capacity is part of the file
	// and wire contract: a hostile document cannot turn class matching into an
	// unbounded selector loop.
	struct StyleClass {
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

	struct StyleDirect {
		uint8_t Properties = 0;

		bool Has(StyleDirectProperty property) const {
			return (Properties & static_cast<uint8_t>(property)) != 0;
		}

		void Set(StyleDirectProperty property) {
			Properties |= static_cast<uint8_t>(property);
		}
	};

	// A declaration group. An empty class is local to the target. A non-empty
	// class matches only that exact stable class name.
	struct StyleRule {
		core::Name Class;
		StyleState State = StyleState::None;
		StyleSet Declarations;
	};

	// An authored UIStyle child. Class is optional for an element-local rule and
	// states are matched only against interaction facts supplied by compile.
	struct UIStyle {
		StyleRule Rule;
	};

	// The collector's explicit reference to a UITheme instance. An entity handle
	// is saved through Store's normal remapping and is never a process pointer.
	struct ThemeBinding {
		ecs::Entity Theme;
	};

	// Viewer-local compile output. It is never authored or replicated; scripts
	// continue to read the direct properties they wrote.
	struct ResolvedStyle {
		StyleSet Values;
		StyleState State = StyleState::None;
	};

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

	struct StyleProvenance {
		static constexpr size_t NO_RULE = static_cast<size_t>(-1);

		core::Name Name;
		StyleValue Value;
		StyleSource Source = StyleSource::Theme;
		core::Name Class;
		StyleState State = StyleState::None;
		// Index in the ResolveStyleTrace rules span, or NO_RULE for theme and
		// direct values. A host can map this back to the authored UIStyle child.
		size_t RuleIndex = NO_RULE;
	};

	struct StyleResolution {
		StyleSet Values;
		std::array<StyleProvenance, StyleSet::MAXIMUM_DECLARATIONS> Provenance{};
		size_t Count = 0;

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
