#include "../src/AccessibilityRouting.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.accessibility_routing")

TEST_CASE("retained accessibility actions require their displayed frame", "[client][accessibility]") {
	using namespace engine;
	gui::RegisterGuiClasses();
	ecs::Store store("accessibility routing");
	gui::InstallGuiServices(store);
	const auto root = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "StarterGui");
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "screen");
	const auto box = store.CreateInstance(gui::GuiClass("TextBox"), "box");
	store.SetParent(screen, root);
	store.SetParent(box, screen);
	gui::Element element;
	element.Size = core::UDim2{0.0f, 80.0f, 0.0f, 40.0f};
	store.Set(box, element);
	store.GetMutable<gui::Label>(box)->Text = "shown";
	gui::CompileRequest request;
	request.Display.Width = 300;
	request.Display.Height = 200;
	gui::Compiled compiled;
	compiled.Rebuild(store, request);
	gui::Router router;
	auto frame = router.CapturePointerFrameWithContinuity(store, compiled.Commands(), 71);
	REQUIRE(frame.Valid());
	client::PresentationFrameMetadata metadata;
	metadata.Store = store.Identity();
	metadata.Runtime = 27;
	metadata.Semantics.emplace();
	client::AccessibilityAction action;
	action.Target = box;
	action.Collector = screen;
	action.WorldEpoch = store.Identity();
	action.FrameSequence = 9;
	action.Action = gui::SemanticAction::Focus;
	const auto check =
		[&](render::SpatialSnapshotStatus status, uint64_t sequence, bool legacy, uint64_t runtime) {
			return client::MatchesAccessibilityOrigin(
				action,
				store,
				compiled.Commands(),
				router,
				71,
				runtime,
				status,
				sequence,
				legacy,
				&frame,
				&metadata
			);
		};
	CHECK(check(render::SpatialSnapshotStatus::Ready, 9, false, 27));
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Ready, 10, false, 27));
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Unavailable, 9, true, 27));
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Unsupported, 9, true, 27));
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Ready, 9, false, 28));
	action.FrameSequence = 0;
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Ready, 9, true, 27));
	CHECK(check(render::SpatialSnapshotStatus::Unsupported, 9, true, 27));
	CHECK(check(render::SpatialSnapshotStatus::NotRequested, 0, true, 27));
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Unavailable, 0, true, 27));
	action.FrameSequence = 9;
	store.GetMutable<gui::Label>(box)->Text = "changed";
	CHECK_FALSE(check(render::SpatialSnapshotStatus::Ready, 9, false, 27));
}
