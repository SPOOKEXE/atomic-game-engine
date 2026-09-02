#pragma once

// Which panel shows a world, when somebody asks to see one.
//
// **The half of "View" that can be silently wrong, in a header for
// `studio/Presentation.hpp`'s reason**: `Editor` needs a window, a device and a
// universe to construct, so a decision made inside it is one no test can reach.
// This one has three ways to be wrong and none of them looks like a fault:
//
// - **Opening a second panel on a world already on screen.** `PresentWorld`
//   round-robins one panel per frame, so two views of one world halve the rate
//   of both. The picture is correct and half as smooth, which reads as the
//   editor being slow rather than as a panel too many.
// - **Taking a panel somebody has pointed at another scene.** That is the editor
//   rearranging a layout on its owner's behalf, and the scene they were watching
//   is simply gone.
// - **Minting a panel when a free one exists.** Every panel is a
//   `render::SceneTarget` and a turn in the rotation, kept for the session.
//
// @tier L13 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/world/World.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace studio {

	// No panel. Also what `ChooseViewportFor` says when one has to be made.
	//
	// **Not zero, because zero is the main viewport** - the one index in this
	// program that is a real panel and reads as "none" to anybody who has met a
	// null handle first.
	//
	// @since v0.14
	inline constexpr size_t NO_VIEWPORT = static_cast<size_t>(-1);

	// One extra viewport panel, as this decision sees it.
	//
	// @since v0.14
	struct PanelView {
		// The world it is pinned to, or an invalid id when it follows the
		// active scene.
		engine::world::WorldId World;

		// Whether the panel exists on screen.
		bool Open = false;
	};

	// The logical canvas and pointer coordinates a game interface receives from
	// one viewport panel. GPU targets may be block-rounded or high-DPI; neither
	// changes authored ScreenGui layout.
	//
	// @since v0.15
	struct ViewportCanvas {
		// The canvas the game interface believes it is laid out on. See the note
		// above: this is the authored size and not the render target's, which
		// may be rounded or scaled.
		//@{
		float Width = 0.0f;
		float Height = 0.0f;
		//@}

		// Where the pointer is on that same canvas, so a hit test inside the
		// panel agrees with the layout the panel drew.
		//@{
		float PointerX = 0.0f;
		float PointerY = 0.0f;
		//@}
	};

	// The pixel extent one viewport asks the renderer to allocate.
	//
	// @since v0.23
	struct ViewportTargetSize {
		uint32_t Width = 1;
		uint32_t Height = 1;
	};

	// Resolves one panel's render extent without changing its aspect ratio.
	// Camera ceilings scale both axes together; clamping them independently
	// makes a wide or tall panel render through a different-shaped lens and the
	// resulting image stretches when it fills the panel. A complete explicit
	// image pair replaces the panel size but follows the same ceiling rule.
	//
	// @param panelWidth    Panel width in display pixels.
	// @param panelHeight   Panel height in display pixels.
	// @param imageWidth    Explicit camera image width, or zero for the panel.
	// @param imageHeight   Explicit camera image height, or zero for the panel.
	// @param maximumWidth  Camera width ceiling, or zero for no ceiling.
	// @param maximumHeight Camera height ceiling, or zero for no ceiling.
	// @return A positive pixel extent with the selected source aspect preserved.
	// @since v0.23
	ViewportTargetSize ResolveViewportTargetSize(
		uint32_t panelWidth,
		uint32_t panelHeight,
		uint32_t imageWidth,
		uint32_t imageHeight,
		uint32_t maximumWidth,
		uint32_t maximumHeight
	);

	// The editor-owned pose of one viewport camera.
	//
	// @since v0.19
	struct ViewportCameraPose {
		// World frame and the editor's decomposed orbit angles.
		//@{
		engine::core::CFrame Frame;
		float Yaw = 0.0f;
		float Pitch = 0.0f;
		//@}
	};

	// The default eye for a world a viewport has not visited before.
	//
	// @since v0.19
	ViewportCameraPose DefaultViewportCamera();

	// Combines fly-camera input in the camera's own basis. Q and E belong to
	// the same basis as WASD, so pitching the camera also pitches its vertical
	// movement instead of leaving it tied to the world's Y axis.
	//
	// @param rotation Camera rotation without translation.
	// @param forward  Signed W/S input.
	// @param right    Signed D/A input.
	// @param up       Signed E/Q input.
	// @return The unnormalised movement vector.
	// @since v0.20
	engine::core::Vector3
	CameraRelativeMovement(const engine::core::CFrame &rotation, float forward, float right, float up);

	// Snaps a viewport camera to a signed world axis. Selecting the axis it is
	// already looking along reverses the view while retaining its screen-up
	// direction, which makes the coincident positive and negative gizmo handles
	// act as a toggle.
	//
	// @param frame     Current camera frame.
	// @param direction Unit signed world axis selected by the gizmo.
	// @return The snapped frame at the same position.
	// @since v0.23
	engine::core::CFrame
	SnapViewportCameraDirection(const engine::core::CFrame &frame, const engine::core::Vector3 &direction);

	// Per-panel, per-world camera memory. This is editor session state only and
	// never enters a world document, snapshot or replication stream.
	//
	// @since v0.19
	class ViewportCameraMemory {
	  public:
		// Saves the current world's pose and restores the destination's. The
		// first world adopts the pose the panel was initialised with.
		void Use(engine::world::WorldId world, ViewportCameraPose &pose);

		// Starts this panel on an explicit pose, used when another viewport asks
		// for a sibling view of what it is already showing.
		void Place(engine::world::WorldId world, const ViewportCameraPose &pose);

	  private:
		engine::world::WorldId Current;
		std::unordered_map<uint32_t, ViewportCameraPose> Remembered;
	};

	// Resolves panel-local game UI coordinates without involving render-target
	// allocation dimensions.
	//
	// @since v0.15
	ViewportCanvas CanvasForViewport(
		float panelX, float panelY, float panelWidth, float panelHeight, float pointerX, float pointerY
	);

	// Which panel should show a world.
	//
	// **A panel already showing it wins over everything**, including a free one:
	// somebody pressing View on a row they are already looking at is asking
	// where it is, not for a second copy.
	//
	// **The main panel is offered only when it would show this world anyway.** It
	// follows the active scene and cannot be pinned, so handing it back for some
	// other world would be a panel that shows the wrong thing the moment anybody
	// clicks a scene.
	//
	// @param world     The world wanted. An invalid id has no panel.
	// @param mainWorld What the main panel draws - the active scene.
	// @param mainOpen  Whether the main panel is on screen.
	// @param panels    The extras, in index order. Index `i` here is panel
	//                  `i + 1`, which is the numbering `Editor::ExtraAt` uses.
	// @return The panel index, or `NO_VIEWPORT` when a new one has to be made.
	// @since v0.14
	size_t ChooseViewportFor(
		engine::world::WorldId world,
		engine::world::WorldId mainWorld,
		bool mainOpen,
		std::span<const PanelView> panels
	);
}
