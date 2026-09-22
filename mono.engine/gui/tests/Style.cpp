#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/gui/Style.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.gui.style")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using namespace engine::gui;

namespace {
	StyleDeclaration Color(std::string_view name, Color3 value) {
		return StyleDeclaration{Name(name), StyleValue::FromColor(value)};
	}
}

TEST_CASE("a style cascade follows theme class local direct order", "[gui][style]") {
	StyleSet theme;
	REQUIRE(theme.Set(Color("BackgroundColor3", Color3{0.1f, 0.1f, 0.1f})));

	StyleClasses classes;
	REQUIRE(classes.Add(Name("primary")));
	REQUIRE(classes.Add(Name("danger")));

	StyleRule primary;
	primary.Class = Name("primary");
	REQUIRE(primary.Declarations.Set(Color("BackgroundColor3", Color3{0.2f, 0.3f, 0.4f})));
	StyleRule danger;
	danger.Class = Name("danger");
	REQUIRE(danger.Declarations.Set(Color("BackgroundColor3", Color3{0.8f, 0.1f, 0.1f})));
	StyleRule local;
	REQUIRE(local.Declarations.Set(Color("TextColor3", Color3{1.0f, 1.0f, 1.0f})));

	StyleSet direct;
	REQUIRE(direct.Set(Color("BackgroundColor3", Color3{0.0f, 0.0f, 0.0f})));
	StyleSet resolved;
	const std::array rules{primary, danger, local};
	REQUIRE(ResolveStyle(theme, classes, rules, direct, StyleState::None, resolved));

	const StyleValue *background = resolved.Find(Name("BackgroundColor3"));
	const StyleValue *text = resolved.Find(Name("TextColor3"));
	REQUIRE(background != nullptr);
	REQUIRE(text != nullptr);
	CHECK(background->Color.R == Approx(0.0f));
	CHECK(text->Color.R == Approx(1.0f));
}

TEST_CASE("a style variant requires each declared interaction fact", "[gui][style]") {
	StyleClasses classes;
	REQUIRE(classes.Add(Name("button")));
	StyleRule pressed;
	pressed.Class = Name("button");
	pressed.State = StyleState::Hovered | StyleState::Pressed;
	REQUIRE(pressed.Declarations.Set(Color("BackgroundColor3", Color3{0.8f, 0.8f, 0.8f})));

	StyleSet resolved;
	REQUIRE(ResolveStyle({}, classes, {&pressed, 1}, {}, StyleState::Hovered, resolved));
	CHECK(resolved.Find(Name("BackgroundColor3")) == nullptr);
	REQUIRE(
		ResolveStyle({}, classes, {&pressed, 1}, {}, StyleState::Hovered | StyleState::Pressed, resolved)
	);
	CHECK(resolved.Find(Name("BackgroundColor3"))->Color.R == Approx(0.8f));
}

TEST_CASE("a style trace identifies each winning cascade layer", "[gui][style]") {
	StyleSet theme;
	REQUIRE(theme.Set(Color("theme", Color3{0.1f, 0.1f, 0.1f})));
	StyleClasses classes;
	REQUIRE(classes.Add(Name("button")));
	StyleRule classRule;
	classRule.Class = Name("button");
	REQUIRE(classRule.Declarations.Set(Color("class", Color3{0.2f, 0.2f, 0.2f})));
	StyleRule local;
	REQUIRE(local.Declarations.Set(Color("local", Color3{0.3f, 0.3f, 0.3f})));
	StyleSet direct;
	REQUIRE(direct.Set(Color("direct", Color3{0.4f, 0.4f, 0.4f})));
	const std::array rules{classRule, local};
	StyleResolution resolved;
	REQUIRE(ResolveStyleTrace(theme, classes, rules, direct, StyleState::None, resolved));
	const auto source = [&](std::string_view name) {
		return std::find_if(
			resolved.Sources().begin(), resolved.Sources().end(), [&](const StyleProvenance &value) {
				return value.Name == Name(name);
			}
		);
	};
	CHECK(source("theme")->Source == StyleSource::Theme);
	CHECK(source("class")->Source == StyleSource::ClassRule);
	CHECK(source("class")->Class == Name("button"));
	CHECK(source("class")->RuleIndex == 0);
	CHECK(source("local")->Source == StyleSource::LocalRule);
	CHECK(source("local")->RuleIndex == 1);
	CHECK(source("direct")->Source == StyleSource::Direct);
	CHECK(source("direct")->RuleIndex == StyleProvenance::NO_RULE);
}

TEST_CASE("style resolution refuses a token whose type changes", "[gui][style]") {
	StyleSet theme;
	REQUIRE(theme.Set(Color("BackgroundColor3", Color3{})));
	StyleSet direct;
	REQUIRE(direct.Set(StyleDeclaration{Name("BackgroundColor3"), StyleValue::FromNumber(1.0f)}));
	StyleSet resolved;
	CHECK_FALSE(ResolveStyle(theme, {}, {}, direct, StyleState::None, resolved));
	CHECK(resolved.Declarations().empty());
}

TEST_CASE("style inputs reject malformed and excessive declarations", "[gui][style][limit]") {
	StyleSet set;
	CHECK_FALSE(set.Set(Color("", Color3{})));
	CHECK_FALSE(set.Set(Color("bad", Color3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f})));
	CHECK_FALSE(set.Set(
		StyleDeclaration{Name("number"), StyleValue::FromNumber(std::numeric_limits<float>::infinity())}
	));
	StyleClasses classes;
	REQUIRE(classes.Add(Name("button")));
	CHECK_FALSE(classes.Add(Name("button")));
	std::array<StyleRule, MAXIMUM_STYLE_RULES + 1> rules{};
	StyleSet resolved;
	CHECK_FALSE(ResolveStyle({}, classes, rules, {}, StyleState::None, resolved));
	CHECK_FALSE(ResolveStyle({}, classes, {}, {}, static_cast<StyleState>(0x80), resolved));
}
