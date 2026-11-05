#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/InputContinuity.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.gui.input_continuity")

TEST_CASE("delayed typing detects changes to its focus, text and composition", "[gui][input]") {
	using namespace engine::gui;
	engine::ecs::Store store("input-continuity");
	RegisterGuiClasses();
	const auto service = InstallGuiServices(store);
	const auto box = store.CreateInstance(GuiClass("TextBox"), "Box");
	Entry entry;
	entry.ClearTextOnFocus = false;
	store.Set(box, entry);
	Label label;
	label.Text = "original";
	store.Set(box, label);
	REQUIRE(Focus(store, box));
	TextCompositionState composition;
	composition.TextBox = box;
	composition.Text = "candidate";
	composition.Start = 1;
	composition.Length = 2;
	composition.Revision = 42;
	store.Set(service, composition);
	const auto witness = CaptureInputContinuity(store, 4096);
	REQUIRE(witness);
	CHECK(witness->RetainedBytes() <= 4096);
	CHECK(MatchesInputContinuity(store, *witness));

	SECTION("a script changes text") {
		store.GetMutable<Label>(box)->Text = "replacement";
	}
	SECTION("the selection changes") {
		store.GetMutable<Entry>(box)->SelectionStart = 1;
	}
	SECTION("the caret changes") {
		store.GetMutable<Entry>(box)->CursorPosition = 1;
	}
	SECTION("editability changes") {
		store.GetMutable<Entry>(box)->TextEditable = false;
	}
	SECTION("multiline behavior changes") {
		store.GetMutable<Entry>(box)->MultiLine = true;
	}
	SECTION("the input method changes") {
		store.GetMutable<TextCompositionState>(service)->Revision++;
	}
	SECTION("the input method text changes without a revision") {
		store.GetMutable<TextCompositionState>(service)->Text = "other";
	}
	SECTION("navigation policy changes") {
		store.GetMutable<GuiServiceState>(service)->AutoSelectGuiEnabled = false;
	}
	SECTION("the focus leaves") {
		REQUIRE(Focus(store, engine::ecs::NULL_ENTITY));
	}
	SECTION("the focused label disappears") {
		store.Remove<Label>(box);
	}
	CHECK_FALSE(MatchesInputContinuity(store, *witness));
}

TEST_CASE("input continuity detects a newly installed GUI service", "[gui][input]") {
	using namespace engine::gui;
	engine::ecs::Store store("input-continuity-service");
	const auto witness = CaptureInputContinuity(store, 4096);
	REQUIRE(witness);
	CHECK(MatchesInputContinuity(store, *witness));
	InstallGuiServices(store);
	CHECK_FALSE(MatchesInputContinuity(store, *witness));
}

TEST_CASE("input continuity is bounded and tied to the original Store", "[gui][input]") {
	using namespace engine::gui;
	engine::ecs::Store store("input-continuity-budget");
	RegisterGuiClasses();
	InstallGuiServices(store);
	const auto box = store.CreateInstance(GuiClass("TextBox"), "Box");
	Entry entry;
	entry.ClearTextOnFocus = false;
	store.Set(box, entry);
	Label label;
	label.Text.assign(256, 'x');
	store.Set(box, label);
	REQUIRE(Focus(store, box));
	CHECK_FALSE(CaptureInputContinuity(store, 255));
	const auto witness = CaptureInputContinuity(store, 4096);
	REQUIRE(witness);
	engine::ecs::Store replacement("input-continuity-budget");
	CHECK_FALSE(MatchesInputContinuity(replacement, *witness));
	store.GetMutable<Label>(box)->Text.assign(256, 'y');
	CHECK(witness->Text == std::string(256, 'x'));
	CHECK_FALSE(MatchesInputContinuity(store, *witness));
}
