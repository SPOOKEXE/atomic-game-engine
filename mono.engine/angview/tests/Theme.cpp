#include <engine/angview/Theme.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.angview.theme")

using namespace engine::angview;

TEST_CASE("presets are named data and the default is true black with white text", "[angview][theme]") {
	const Theme dark = Theme::Dark();
	CHECK(dark.Background == Rgba{0.0f, 0.0f, 0.0f, 1.0f});
	CHECK(dark.Text == Rgba{1.0f, 1.0f, 1.0f, 1.0f});
	for (const std::string &name : Theme::PresetNames()) {
		const auto preset = Theme::Preset(name);
		REQUIRE(preset.has_value());
		CHECK(preset->Name == name);
	}
	CHECK(!Theme::Preset("Neon").has_value());
	CHECK(!Theme::Minimal().ShowGrid);
	CHECK(!Theme::Minimal().ShowHeaders);
	CHECK(Theme::Blueprint().Wires == WireStyle::Orthogonal);
	CHECK(Theme::HighContrast().BorderWidth > dark.BorderWidth);
}

TEST_CASE("port shapes fall back to the default and colours mix linearly", "[angview][theme]") {
	Theme theme = Theme::Dark();
	theme.PortShapes["image"] = PortShape::Square;
	CHECK(theme.ShapeFor("image") == PortShape::Square);
	CHECK(theme.ShapeFor("number") == PortShape::Circle);
	const Rgba half = Mix({0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.5f);
	CHECK(half.R == 0.5f);
	CHECK(Mix({}, {1.0f, 1.0f, 1.0f, 1.0f}, 9.0f).R == 1.0f);
	CHECK(FromColor3({0.2f, 0.4f, 0.6f}).G == 0.4f);
}
