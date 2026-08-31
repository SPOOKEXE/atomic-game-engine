#include <engine/render/Overlay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/SettingsMenu.hpp>

TEST_SUITE_ID("client.settingsmenu")

using client::Options;
using client::SettingsMenu;
using client::SettingsMenuResult;

TEST_CASE("the settings menu toggles live presentation features", "[client][settings]") {
	Options settings;
	SettingsMenu menu;

	menu.Toggle();
	REQUIRE(menu.IsOpen());
	REQUIRE(menu.Selected() == 0);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Changed);
	CHECK_FALSE(settings.EnableEditableMeshes);

	menu.Move(1);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Changed);
	CHECK_FALSE(settings.EnableEditableImages);

	menu.Move(1);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Changed);
	CHECK_FALSE(settings.EnableParticles);

	menu.Move(1);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Changed);
	CHECK_FALSE(settings.EnablePostProcessing);
}

TEST_CASE("settings navigation wraps and resume closes the menu", "[client][settings]") {
	Options settings;
	SettingsMenu menu;
	menu.Toggle();

	menu.Move(-1);
	REQUIRE(menu.Selected() == SettingsMenu::ROWS - 1);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Quit);
	CHECK(menu.IsOpen());

	menu.Move(-1);
	REQUIRE(menu.Activate(settings) == SettingsMenuResult::Closed);
	CHECK_FALSE(menu.IsOpen());
}

TEST_CASE("an open settings menu draws into the host overlay", "[client][settings]") {
	Options settings;
	SettingsMenu menu;
	engine::render::OverlayImage image;
	image.Resize(640, 480);

	client::DrawSettingsMenu(image, settings, menu);
	CHECK_FALSE(image.HasContent());

	menu.Toggle();
	client::DrawSettingsMenu(image, settings, menu);
	CHECK(image.HasContent());
	CHECK(image.IsDirty());
}
