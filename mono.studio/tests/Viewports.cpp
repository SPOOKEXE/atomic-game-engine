// Which panel a "View" press lands in.
//
// **The bug this exists for is the one you cannot see: a view with no way
// back.** A viewport pinned to a client and then closed was recoverable only
// because the replica had a row among the scenes, and the *server's* view - the
// scene itself, drawn by the main panel - had no row anywhere. Live Instances is
// the list, and this is the decision behind its buttons.
//
// Its three failure modes all look like something other than a fault: a second
// panel on one world halves the refresh rate of both and reads as the editor
// being slow; a panel taken from another scene reads as that scene closing; and
// a panel minted while a free one sat there is a `SceneTarget` and a turn in the
// rotation nobody asked to pay.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <studio/Viewports.hpp>

TEST_SUITE_ID("studio.viewports")

using engine::world::WorldId;
using studio::CanvasForViewport;
using studio::ChooseViewportFor;
using studio::DefaultViewportCamera;
using studio::NO_VIEWPORT;
using studio::PanelView;
using studio::ViewportCameraMemory;
using studio::ViewportCameraPose;

namespace {
	// Three scenes and a client view, as an editor mid-play holds them.
	constexpr WorldId SCENE{0};
	constexpr WorldId OTHER{1};
	constexpr WorldId CLIENT{2};
}

TEST_CASE(
	"game UI uses each panel's logical size instead of its GPU allocation", "[studio][viewports][gui]"
) {
	const studio::ViewportCanvas left = CanvasForViewport(12.0f, 30.0f, 841.0f, 674.0f, 432.0f, 367.0f);
	CHECK(left.Width == 841.0f);
	CHECK(left.Height == 674.0f);
	CHECK(left.PointerX == 420.0f);
	CHECK(left.PointerY == 337.0f);

	const studio::ViewportCanvas right = CanvasForViewport(853.0f, 30.0f, 729.0f, 674.0f, 1217.5f, 367.0f);
	CHECK(right.Width == 729.0f);
	CHECK(right.Height == 674.0f);
	CHECK(right.PointerX == 364.5f);
	CHECK(right.PointerY == 337.0f);
}

TEST_CASE("a world already on screen is found rather than opened twice", "[studio][viewports]") {
	const std::array<PanelView, 2> panels{PanelView{CLIENT, true}, PanelView{OTHER, true}};

	// The extra that is pinned to it. Index 1 here is panel 2, which is the
	// numbering `Editor::ExtraAt` uses and the one an off-by-one would swap.
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, panels) == 1);
	CHECK(ChooseViewportFor(OTHER, SCENE, true, panels) == 2);

	// The main panel, which is not pinned to anything - it draws the active
	// scene, so "showing SCENE" is a fact about `mainWorld` rather than about
	// the panel.
	CHECK(ChooseViewportFor(SCENE, SCENE, true, panels) == 0);

	// **Closed and it is still the panel this world belongs in.** A server view
	// shut from its title bar comes back as the main panel rather than as an
	// extra pinned over the top of it, which is the whole reason the closed case
	// is separate from the free-panel search below.
	CHECK(ChooseViewportFor(SCENE, SCENE, false, panels) == 0);

	// And nothing has a panel for a world that is not one.
	CHECK(ChooseViewportFor(WorldId{}, SCENE, true, panels) == NO_VIEWPORT);
}

TEST_CASE("a panel somebody pinned is never taken", "[studio][viewports]") {
	// One free panel: closed. A closed panel is free whatever it is pinned to -
	// nobody is looking at it.
	const std::array<PanelView, 3> panels{
		PanelView{OTHER, true}, PanelView{CLIENT, false}, PanelView{SCENE, true}
	};

	// **Not panel 1 and not panel 3**, which are showing scenes of their own,
	// and not the main panel, which is showing a third.
	CHECK(ChooseViewportFor(WorldId{7}, SCENE, true, panels) == 2);

	// An open panel following the active scene rather than pinned to one is
	// free too: nobody has said it must show anything in particular.
	const std::array<PanelView, 2> loose{PanelView{OTHER, true}, PanelView{WorldId{}, true}};
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, loose) == 2);

	// With every panel spoken for the honest answer is "make one" rather than
	// evicting somebody - `Editor::ShowWorldInViewport` is what mints it.
	const std::array<PanelView, 2> full{PanelView{OTHER, true}, PanelView{CLIENT, true}};
	CHECK(ChooseViewportFor(WorldId{7}, SCENE, true, full) == NO_VIEWPORT);

	// Including when there are no extras at all, which is what a fresh editor
	// with one panel open looks like.
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, std::span<const PanelView>{}) == NO_VIEWPORT);
}

TEST_CASE("new worlds start above the scene looking at the origin", "[studio][viewports][camera]") {
	const ViewportCameraPose pose = DefaultViewportCamera();
	CHECK(pose.Frame.Position == engine::core::Vector3{0.0f, 30.0f, 30.0f});
	CHECK(pose.Frame.LookVector().Dot((engine::core::Vector3::Zero - pose.Frame.Position).Unit()) > 0.9999f);
	const engine::core::CFrame rebuilt = engine::core::CFrame::Angles(pose.Pitch, pose.Yaw, 0.0f);
	CHECK(rebuilt.LookVector().Dot(pose.Frame.LookVector()) > 0.9999f);
}

TEST_CASE("each viewport remembers an independent camera per world", "[studio][viewports][camera]") {
	ViewportCameraMemory left;
	ViewportCameraMemory right;
	ViewportCameraPose leftPose = DefaultViewportCamera();
	ViewportCameraPose rightPose = DefaultViewportCamera();
	left.Use(SCENE, leftPose);
	right.Use(SCENE, rightPose);

	leftPose.Frame.Position = {1.0f, 2.0f, 3.0f};
	leftPose.Yaw = 0.25f;
	left.Use(OTHER, leftPose);
	CHECK(leftPose.Frame.Position == engine::core::Vector3{0.0f, 30.0f, 30.0f});

	rightPose.Frame.Position = {40.0f, 50.0f, 60.0f};
	right.Use(OTHER, rightPose);
	left.Use(SCENE, leftPose);
	right.Use(SCENE, rightPose);

	CHECK(leftPose.Frame.Position == engine::core::Vector3{1.0f, 2.0f, 3.0f});
	CHECK(leftPose.Yaw == 0.25f);
	CHECK(rightPose.Frame.Position == engine::core::Vector3{40.0f, 50.0f, 60.0f});

	// Closing a panel has no world to use, but it must retain the pose of the
	// world that was visible. Reopening the same world restores that pose from
	// session memory rather than starting a new camera.
	leftPose.Frame.Position = {7.0f, 8.0f, 9.0f};
	left.Use(WorldId{}, leftPose);
	leftPose = DefaultViewportCamera();
	left.Use(SCENE, leftPose);
	CHECK(leftPose.Frame.Position == engine::core::Vector3{7.0f, 8.0f, 9.0f});
}
