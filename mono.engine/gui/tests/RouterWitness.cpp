#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.gui.router_witness")

TEST_CASE("router witness tracks capture and host incarnation", "[gui][input][continuity]") {
	using namespace engine::gui;
	engine::ecs::Store store("router-witness");
	RegisterGuiClasses();
	InstallGuiServices(store);
	const auto screen = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	const auto button = store.CreateInstance(GuiClass("TextButton"), "button");
	REQUIRE(store.SetParent(button, screen));
	Element element;
	element.Active = true;
	store.Set(button, element);
	Resolved resolved;
	resolved.AbsolutePosition = {0.0f, 0.0f};
	resolved.AbsoluteSize = {100.0f, 100.0f};
	resolved.Clip = {0.0f, 0.0f, 100.0f, 100.0f};
	resolved.Rendered = true;
	store.Set(button, resolved);
	DrawList list;
	DrawCommand command;
	command.Source = button;
	command.Collector = screen;
	command.Bounds = resolved.Clip;
	command.Clip = resolved.Clip;
	list.Commands.push_back(command);
	Router router;
	auto empty = router.CaptureRouterContinuity(store, 1024 * 1024, 11);
	REQUIRE(empty.Valid());
	CHECK(router.MatchesRouterContinuity(store, empty, 11));
	CHECK_FALSE(router.MatchesRouterContinuity(store, empty, 12));
	CHECK_FALSE(router.CaptureRouterContinuity(store, 1024 * 1024, 0).Valid());
	CHECK_FALSE(router.CaptureRouterContinuity(store, 1, 11).Valid());
	Pointer pointer;
	pointer.Position = {50.0f, 50.0f};
	pointer.Down = true;
	(void)router.Update(store, list, pointer);
	REQUIRE(router.Pressed() == button);
	CHECK_FALSE(router.MatchesRouterContinuity(store, empty, 11));
	auto held = router.CaptureRouterContinuity(store, 1024 * 1024, 11);
	REQUIRE(held.Valid());
	CHECK(router.MatchesRouterContinuity(store, held, 11));
	CHECK(router.CaptureRouterContinuity(store, held.RetainedBytes(), 11).Valid());
	CHECK_FALSE(router.CaptureRouterContinuity(store, held.RetainedBytes() - 1, 11).Valid());
	pointer.Down = false;
	(void)router.Update(store, list, pointer);
	CHECK_FALSE(router.MatchesRouterContinuity(store, held, 11));
}
