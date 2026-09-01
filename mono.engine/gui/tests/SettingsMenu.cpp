#include <engine/gui/SettingsMenu.hpp>

#include <catch2/catch_test_macros.hpp>

using engine::core::Name;
using engine::ecs::Store;
using engine::gui::ReachSettingsMenuExtensions;
using engine::gui::SettingsMenuActionsOf;
using engine::gui::SettingsMenuExtensions;

TEST_CASE("settings menu actions retain insertion order and replace by stable name", "[gui][settings]") {
	Store store("settings_menu_test");
	SettingsMenuExtensions &extensions = ReachSettingsMenuExtensions(store);

	CHECK(extensions.Set(Name("respawn"), "Respawn"));
	CHECK(extensions.Set(Name("photo"), "Photo Mode"));
	CHECK(extensions.Set(Name("respawn"), "Respawn Character"));

	const auto actions = SettingsMenuActionsOf(store);
	REQUIRE(actions.size() == 2);
	CHECK(actions[0].Id == Name("respawn"));
	CHECK(actions[0].Label == "Respawn Character");
	CHECK(actions[1].Id == Name("photo"));
}

TEST_CASE("settings menu action bounds reject ambiguous or unbounded rows", "[gui][settings]") {
	SettingsMenuExtensions extensions;
	CHECK_FALSE(extensions.Set(Name{}, "Missing id"));
	CHECK_FALSE(extensions.Set(Name("missing-label"), ""));
	CHECK_FALSE(
		extensions.Set(Name("long"), std::string(SettingsMenuExtensions::MAXIMUM_LABEL_BYTES + 1, 'x'))
	);

	for (size_t index = 0; index < SettingsMenuExtensions::MAXIMUM_ACTIONS; index++) {
		CHECK(extensions.Set(Name("action-" + std::to_string(index)), "Action"));
	}
	CHECK_FALSE(extensions.Set(Name("one-too-many"), "Action"));
	CHECK(extensions.Remove(Name("action-0")));
	CHECK_FALSE(extensions.Remove(Name("action-0")));
	CHECK(extensions.Set(Name("replacement"), "Replacement"));

	extensions.Clear();
	CHECK(extensions.Actions().empty());
}
