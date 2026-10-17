#include <engine/gui/Style.hpp>

#include <algorithm>
#include <cmath>

namespace engine::gui {

	bool StyleSet::Set(StyleDeclaration declaration) {
		if (!declaration.Name.IsValid()) {
			return false;
		}
		switch (declaration.Value.Type) {
		case StyleValueType::Color:
			if (!std::isfinite(declaration.Value.Color.R) || !std::isfinite(declaration.Value.Color.G) ||
				!std::isfinite(declaration.Value.Color.B)) {
				return false;
			}
			break;
		case StyleValueType::Number:
			if (!std::isfinite(declaration.Value.Number)) {
				return false;
			}
			break;
		default:
			return false;
		}
		for (size_t index = 0; index < Count; index++) {
			StyleDeclaration &existing = Values[index];
			if (existing.Name != declaration.Name) {
				continue;
			}
			if (existing.Value.Type != declaration.Value.Type) {
				return false;
			}
			existing = declaration;
			RevisionNumber++;
			return true;
		}

		if (Count == Values.size()) {
			return false;
		}
		Values[Count++] = declaration;
		RevisionNumber++;
		return true;
	}

	const StyleValue *StyleSet::Find(core::Name name) const {
		const auto found = std::find_if(
			Values.begin(),
			Values.begin() + static_cast<ptrdiff_t>(Count),
			[name](const StyleDeclaration &declaration) { return declaration.Name == name; }
		);
		return found == Values.begin() + static_cast<ptrdiff_t>(Count) ? nullptr : &found->Value;
	}

	std::span<const StyleDeclaration> StyleSet::Declarations() const {
		return std::span<const StyleDeclaration>(Values.data(), Count);
	}

	bool StyleClasses::Add(core::Name name) {
		if (!name.IsValid() || Count == Values.size() ||
			std::find(Values.begin(), Values.begin() + static_cast<ptrdiff_t>(Count), name) !=
				Values.begin() + static_cast<ptrdiff_t>(Count)) {
			return false;
		}
		Values[Count++] = name;
		return true;
	}

	std::span<const core::Name> StyleClasses::Names() const {
		return std::span<const core::Name>(Values.data(), Count);
	}

	namespace {
		bool Apply(
			const StyleSet &from,
			StyleSource source,
			core::Name className,
			StyleState state,
			size_t ruleIndex,
			StyleResolution &into
		) {
			for (const StyleDeclaration &declaration : from.Declarations()) {
				if (!into.Values.Set(declaration)) {
					return false;
				}
				auto provenance = std::find_if(
					into.Provenance.begin(),
					into.Provenance.begin() + static_cast<std::ptrdiff_t>(into.Count),
					[&](const StyleProvenance &value) { return value.Name == declaration.Name; }
				);
				if (provenance == into.Provenance.begin() + static_cast<std::ptrdiff_t>(into.Count)) {
					if (into.Count == into.Provenance.size()) {
						return false;
					}
					provenance = into.Provenance.begin() + static_cast<std::ptrdiff_t>(into.Count++);
				}
				*provenance = {declaration.Name, declaration.Value, source, className, state, ruleIndex};
			}
			return true;
		}

	}

	bool ResolveStyle(
		const StyleSet &theme,
		const StyleClasses &classes,
		std::span<const StyleRule> rules,
		const StyleSet &direct,
		StyleState state,
		StyleSet &out
	) {
		StyleResolution resolved;
		if (!ResolveStyleTrace(theme, classes, rules, direct, state, resolved)) {
			return false;
		}
		out = resolved.Values;
		return true;
	}

	bool ResolveStyleTrace(
		const StyleSet &theme,
		const StyleClasses &classes,
		std::span<const StyleRule> rules,
		const StyleSet &direct,
		StyleState state,
		StyleResolution &out
	) {
		if (rules.size() > MAXIMUM_STYLE_RULES ||
			(static_cast<uint8_t>(state) & ~static_cast<uint8_t>(0x3f)) != 0) {
			return false;
		}
		StyleResolution resolved;
		if (!Apply(theme, StyleSource::Theme, {}, StyleState::None, StyleProvenance::NO_RULE, resolved)) {
			return false;
		}

		for (const core::Name className : classes.Names()) {
			for (size_t ruleIndex = 0; ruleIndex < rules.size(); ruleIndex++) {
				const StyleRule &rule = rules[ruleIndex];
				if (rule.Class != className || !Includes(state, rule.State)) {
					continue;
				}
				if (!Apply(
						rule.Declarations, StyleSource::ClassRule, rule.Class, rule.State, ruleIndex, resolved
					)) {
					return false;
				}
			}
		}

		for (size_t ruleIndex = 0; ruleIndex < rules.size(); ruleIndex++) {
			const StyleRule &rule = rules[ruleIndex];
			if (rule.Class.IsValid() || !Includes(state, rule.State)) {
				continue;
			}
			if (!Apply(rule.Declarations, StyleSource::LocalRule, {}, rule.State, ruleIndex, resolved)) {
				return false;
			}
		}

		if (!Apply(direct, StyleSource::Direct, {}, StyleState::None, StyleProvenance::NO_RULE, resolved)) {
			return false;
		}
		out = resolved;
		return true;
	}
}
