#include "PointerFrameFacts.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.gui.pointerframefacts")

using namespace engine;
using namespace engine::gui;

TEST_CASE("a pointer frame retains its own routing facts after the source changes", "[gui][pointer]") {
	detail::PointerFrameFacts frame;
	ecs::Entity collector, button;
	{
		RegisterGuiClasses();
		ecs::Store store("frame facts");
		collector = store.CreateInstance(GuiClass("ScreenGui"), "surface");
		button = store.CreateInstance(GuiClass("TextButton"), "button");
		REQUIRE(collector != ecs::NULL_ENTITY);
		REQUIRE(button != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(button, collector));
		SpatialCanvas spatial;
		spatial.Interactive = true;
		spatial.Size = {320, 200};
		store.Set<SpatialCanvas>(collector, spatial);
		DrawList list;
		list.CanvasSize = {800, 600};
		DrawCommand command;
		command.Source = button;
		command.Collector = collector;
		command.Bounds = {{0, 0}, {100, 100}};
		command.Clip = {{0, 0}, {100, 100}};
		command.Key = "original";
		list.Commands.push_back(command);
		REQUIRE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
		list.Commands.clear();
		store.DestroyInstance(button);
	}
	REQUIRE(frame.Valid);
	CHECK(frame.CanvasSize == core::Vector2{800, 600});
	REQUIRE(frame.SpatialCanvases.size() == 1);
	CHECK(frame.SpatialCanvases[0].Collector == collector);
	CHECK(frame.SpatialCanvases[0].Canvas.Size == core::Vector2{320, 200});
	Pointer pointer;
	pointer.Position = {25, 25};
	pointer.Collector = collector;
	pointer.Down = true;
	const auto sample = detail::SamplePointerFrameFacts(frame, pointer);
	REQUIRE(sample.Valid);
	CHECK(sample.Hovered == button);
	REQUIRE(sample.Hits.size() == 1);
	CHECK(sample.Hits[0].Key == "original");
	pointer.Inside = false;
	const auto outside = detail::SamplePointerFrameFacts(frame, pointer);
	REQUIRE(outside.Valid);
	CHECK(outside.Hovered == ecs::NULL_ENTITY);
	pointer.Inside = true;
	frame.ModalOverflow = true;
	const auto overflow = detail::SamplePointerFrameFacts(frame, pointer);
	REQUIRE(overflow.Valid);
	CHECK(overflow.Hovered == ecs::NULL_ENTITY);
}

TEST_CASE("a pointer frame refuses excessive command and key payloads", "[gui][pointer]") {
	ecs::Store store("bounded frame facts");
	DrawList list;
	detail::PointerFrameFacts frame;
	list.Commands.resize(16385);
	CHECK_FALSE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
	CHECK_FALSE(frame.Valid);
	list.Commands.resize(1);
	list.Commands[0].Key.assign(16 * 1024 * 1024, 'x');
	CHECK_FALSE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
	CHECK_FALSE(frame.Valid);
}
TEST_CASE("pointer frames own and bound textbox focus text", "[gui][pointer]") {
	RegisterGuiClasses();
	ecs::Store store("entry frame");
	const auto collector = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	const auto entry = store.CreateInstance(GuiClass("TextBox"), "entry");
	REQUIRE(store.SetParent(entry, collector));
	auto *label = store.GetMutable<Label>(entry);
	auto *settings = store.GetMutable<Entry>(entry);
	REQUIRE(label);
	REQUIRE(settings);
	label->Text = "original";
	settings->ClearTextOnFocus = true;
	DrawList list;
	DrawCommand command;
	command.Source = entry;
	command.Collector = collector;
	command.Bounds = {{0, 0}, {100, 100}};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);
	detail::PointerFrameFacts frame;
	REQUIRE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
	label->Text = "changed";
	settings->ClearTextOnFocus = false;
	Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	const auto sample = detail::SamplePointerFrameFacts(frame, pointer);
	REQUIRE(sample.Valid);
	CHECK(sample.VisualEntryClearOnFocus);
	CHECK(sample.VisualEntryHasLabel);
	CHECK(sample.VisualEntryText == "original");
	label->Text.assign(detail::MAX_POINTER_ENTRY_TEXT_BYTES + 1, 'x');
	CHECK_FALSE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
	CHECK_FALSE(frame.Valid);
}
TEST_CASE("frame facts own directional selection overrides", "[gui][pointer]") {
	RegisterGuiClasses();
	ecs::Store store("navigation frame");
	auto collector = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	auto current = store.CreateInstance(GuiClass("TextButton"), "current");
	auto target = store.CreateInstance(GuiClass("TextButton"), "target");
	REQUIRE(store.SetParent(current, collector));
	REQUIRE(store.SetParent(target, collector));
	Selection selected;
	selected.NextDown = target;
	selected.Order = 17;
	store.Set<Selection>(current, selected);
	DrawList list;
	DrawCommand command;
	command.Source = current;
	command.Collector = collector;
	command.Bounds = {{0, 0}, {100, 100}};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);
	detail::PointerFrameFacts frame;
	REQUIRE(detail::CapturePointerFrameFacts(store, list, {}, false, frame));
	auto *live = store.GetMutable<Selection>(current);
	REQUIRE(live);
	live->Order = 0;
	live->NextDown = {};
	const auto *saved = frame.SelectionOf(current);
	REQUIRE(saved);
	CHECK(saved->Order == 17);
	CHECK(saved->NextDown == target);
}

TEST_CASE(
	"frame layer expectations include non-drawn input and visible spatial collectors", "[gui][pointer]"
) {
	RegisterGuiClasses();
	ecs::Store store("frame layer expectations");
	auto screen = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	auto scroll = store.CreateInstance(GuiClass("ScrollingFrame"), "scroll");
	REQUIRE(store.SetParent(scroll, screen));
	store.Set<Scrolling>(scroll, {});
	store.Set<ScrollState>(scroll, {});
	store.Set<Resolved>(scroll, {});
	DrawList list;
	Router router;
	auto screenFrame = router.CapturePointerFrame(store, list);
	REQUIRE(screenFrame.Valid());
	CHECK(screenFrame.ExpectsScreenLayer());
	CHECK_FALSE(screenFrame.ExpectsSpatialLayer());
	auto spatial = store.CreateInstance(GuiClass("SurfaceGui"), "surface");
	SpatialCanvas canvas;
	canvas.Visible = true;
	store.Set<SpatialCanvas>(spatial, canvas);
	auto spatialFrame = router.CapturePointerFrame(store, list);
	REQUIRE(spatialFrame.Valid());
	CHECK(spatialFrame.ExpectsScreenLayer());
	CHECK(spatialFrame.ExpectsSpatialLayer());
	canvas.Visible = false;
	store.Set<SpatialCanvas>(spatial, canvas);
	auto hiddenFrame = router.CapturePointerFrame(store, list);
	REQUIRE(hiddenFrame.Valid());
	CHECK_FALSE(hiddenFrame.ExpectsSpatialLayer());
}

TEST_CASE("frame reports exact command layers and rejects mismatched classification", "[gui][pointer]") {
	RegisterGuiClasses();
	ecs::Store store("frame command layers");
	auto screen = store.CreateInstance(GuiClass("ScreenGui"), "screen");
	auto surface = store.CreateInstance(GuiClass("SurfaceGui"), "surface");
	SpatialCanvas canvas;
	canvas.Visible = true;
	store.Set<SpatialCanvas>(surface, canvas);
	DrawList list;
	DrawCommand command;
	command.Collector = screen;
	command.Source = screen;
	command.Transparency = 1.0f;
	list.Commands.push_back(command);
	Router router;
	auto screenFrame = router.CapturePointerFrame(store, list);
	REQUIRE(screenFrame.Valid());
	CHECK(screenFrame.HasScreenCommands());
	CHECK_FALSE(screenFrame.HasSpatialCommands());
	CHECK(screenFrame.LayerClassificationValid());
	list.Commands[0].Collector = surface;
	list.Commands[0].Source = surface;
	list.Commands[0].Spatial = true;
	auto spatialFrame = router.CapturePointerFrame(store, list);
	REQUIRE(spatialFrame.Valid());
	CHECK_FALSE(spatialFrame.HasScreenCommands());
	CHECK(spatialFrame.HasSpatialCommands());
	CHECK(spatialFrame.LayerClassificationValid());
	list.Commands[0].Spatial = false;
	auto mismatch = router.CapturePointerFrame(store, list);
	REQUIRE(mismatch.Valid());
	CHECK(mismatch.HasScreenCommands());
	CHECK_FALSE(mismatch.LayerClassificationValid());
}
