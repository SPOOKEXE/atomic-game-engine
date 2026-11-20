#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Capabilities.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.script.capabilities")

TEST_CASE("only a server Script is warned for GUI input on game UI", "[script][capabilities]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("capabilities");
	scene::InstallServices(store);
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
	REQUIRE(store.SetParent(screen, store.FindFirstRoot("StarterGui")));
	const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "Go");
	REQUIRE(store.SetParent(button, screen));
	const auto widget = store.CreateInstance(gui::GuiClass("DockWidgetPluginGui"), "Tool");
	const auto toolButton = store.CreateInstance(gui::GuiClass("TextButton"), "Run");
	REQUIRE(store.SetParent(toolButton, widget));

	const script::HostRole server{true, false, false};
	const script::HostRole client{false, true, false};
	const script::HostRole both{true, true, false};
	const script::HostRole editPlugin{false, false, true};

	CHECK_FALSE(
		script::SignalCapabilityWarning(store, server, script::SignalKind::GuiActivated, button).empty()
	);
	CHECK_FALSE(
		script::SignalCapabilityWarning(store, server, script::SignalKind::GuiMouseEnter, button).empty()
	);
	CHECK(script::SignalCapabilityWarning(store, client, script::SignalKind::GuiActivated, button).empty());
	CHECK(script::SignalCapabilityWarning(store, both, script::SignalKind::GuiActivated, button).empty());
	CHECK(
		script::SignalCapabilityWarning(store, editPlugin, script::SignalKind::GuiActivated, button).empty()
	);

	// A plugin's own widgets are its input even when it runs as the server.
	CHECK(
		script::SignalCapabilityWarning(store, server, script::SignalKind::GuiActivated, toolButton).empty()
	);

	// Signals that are not player input are untouched.
	CHECK(script::SignalCapabilityWarning(store, server, script::SignalKind::Changed, button).empty());
}
