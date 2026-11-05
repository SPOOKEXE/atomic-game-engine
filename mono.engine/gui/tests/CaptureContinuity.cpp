#include "CaptureContinuity.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_SUITE_ID("engine.gui.router_capture_continuity")

TEST_CASE("router continuity tracks virtual capture and modal restoration", "[gui][input][continuity]") {
	using namespace engine::gui;
	using namespace engine::gui::detail;
	engine::ecs::Store store("capture-continuity");
	RegisterGuiClasses();
	const auto collector = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	const auto box = store.CreateInstance(GuiClass("TextBox"), "box");
	const auto button = store.CreateInstance(GuiClass("TextButton"), "button");
	const auto canvas = store.CreateInstance(GuiClass("ScrollingFrame"), "canvas");
	Entry entry;
	entry.ClearTextOnFocus = true;
	store.Set(box, entry);
	Label label;
	label.Text = "saved text";
	store.Set(box, label);
	Element element;
	element.Visible = element.Selectable = element.Interactable = true;
	store.Set(button, element);
	ScrollMotion motion;
	motion.Held = true;
	motion.ReleasedAt = 3.25;
	store.Set(canvas, motion);
	std::string key = "virtual-item";
	std::array modals{CaptureOwner{button, collector}};
	std::array savedFocus{CaptureOwner{box, collector}};
	std::array savedSelection{CaptureOwner{button, collector}};
	CaptureState state;
	state.Holding = {button, collector};
	state.HoldingCollection = collector;
	state.HoldingKey = key;
	state.HoldingIndex = 7;
	state.Canvas = {canvas, collector};
	state.Modals = modals;
	state.SavedFocus = savedFocus;
	state.SavedSelection = savedSelection;
	const auto witness = CaptureRouterContinuity(store, state, 1024 * 1024, 7);
	REQUIRE(witness);
	CHECK(MatchesRouterContinuity(store, *witness, state, 7));
	CHECK_FALSE(MatchesRouterContinuity(store, *witness, state, 8));
	CHECK_FALSE(CaptureRouterContinuity(store, state, 1024 * 1024, 0));
	CHECK(CaptureRouterContinuity(store, state, witness->RetainedBytes, 7));
	CHECK_FALSE(CaptureRouterContinuity(store, state, witness->RetainedBytes - 1, 7));
	SECTION("virtual identity changes") {
		key[0] = 'V';
	}
	SECTION("the virtual index changes") {
		state.HoldingIndex++;
	}
	SECTION("modal context changes") {
		modals[0].Target = box;
	}
	SECTION("modal overflow changes") {
		state.ModalOverflow = true;
	}
	SECTION("saved focus text changes") {
		store.GetMutable<Label>(box)->Text = "changed";
	}
	SECTION("saved focus behavior changes") {
		store.GetMutable<Entry>(box)->ClearTextOnFocus = false;
	}
	SECTION("saved selection eligibility changes") {
		store.GetMutable<Element>(button)->Selectable = false;
	}
	SECTION("canvas motion changes") {
		store.GetMutable<ScrollMotion>(canvas)->ReleasedAt = 4.0;
	}
	SECTION("canvas pull changes") {
		store.GetMutable<ScrollMotion>(canvas)->Pull.X = 4.0f;
	}
	CHECK_FALSE(MatchesRouterContinuity(store, *witness, state, 7));
}

TEST_CASE("router continuity accounts for empty and long keys", "[gui][input][continuity]") {
	using namespace engine::gui::detail;
	engine::ecs::Store store("capture-continuity-key");
	CaptureState state;
	for (const std::string &key : {std::string{}, std::string{"a"}, std::string(65536, 'x')}) {
		state.HoldingKey = key;
		const auto witness = CaptureRouterContinuity(store, state, 1024 * 1024, 7);
		REQUIRE(witness);
		CHECK(CaptureRouterContinuity(store, state, witness->RetainedBytes, 7));
		CHECK_FALSE(CaptureRouterContinuity(store, state, witness->RetainedBytes - 1, 7));
	}
	const std::string tooLarge(65537, 'x');
	state.HoldingKey = tooLarge;
	CHECK_FALSE(CaptureRouterContinuity(store, state, 1024 * 1024, 7));
}
