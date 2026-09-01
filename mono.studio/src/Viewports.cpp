#include <studio/Viewports.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::Vector3;
	using engine::world::WorldId;

	ViewportCameraPose DefaultViewportCamera() {
		ViewportCameraPose pose;
		pose.Frame = CFrame::LookAt(Vector3{0.0f, 30.0f, 30.0f}, Vector3::Zero);
		const Vector3 angles = pose.Frame.ToAngles();
		pose.Pitch = angles.X;
		pose.Yaw = angles.Y;
		return pose;
	}

	Vector3
	CameraRelativeMovement(const CFrame &rotation, const float forward, const float right, const float up) {
		return rotation.LookVector() * forward + rotation.RightVector() * right + rotation.UpVector() * up;
	}

	void ViewportCameraMemory::Use(WorldId world, ViewportCameraPose &pose) {
		if (world == Current) {
			return;
		}

		if (Current.IsValid()) {
			Remembered[Current.Index] = pose;
		}
		if (!world.IsValid()) {
			Current = {};
			return;
		}

		if (!Current.IsValid() && Remembered.empty()) {
			Current = world;
			Remembered[world.Index] = pose;
			return;
		}

		Current = world;
		const auto found = Remembered.find(world.Index);
		pose = found == Remembered.end() ? DefaultViewportCamera() : found->second;
		Remembered[world.Index] = pose;
	}

	void ViewportCameraMemory::Place(WorldId world, const ViewportCameraPose &pose) {
		Current = world;
		if (world.IsValid()) {
			Remembered[world.Index] = pose;
		}
	}

	ViewportCanvas CanvasForViewport(
		float panelX, float panelY, float panelWidth, float panelHeight, float pointerX, float pointerY
	) {
		return ViewportCanvas{
			panelWidth,
			panelHeight,
			pointerX - panelX,
			pointerY - panelY,
		};
	}

	size_t
	ChooseViewportFor(WorldId world, WorldId mainWorld, bool mainOpen, std::span<const PanelView> panels) {
		if (!world.IsValid()) {
			return NO_VIEWPORT;
		}

		// Already on screen, main panel first - it is the one somebody means by
		// "the view" when the world is the scene they are editing.
		if (mainOpen && mainWorld == world) {
			return 0;
		}

		for (size_t index = 0; index < panels.size(); index++) {
			if (panels[index].Open && panels[index].World == world) {
				return index + 1;
			}
		}

		// **The main panel, reopened, when this world is what it draws.** A
		// server view shut from its title bar comes back here rather than as an
		// extra pinned over the top of the panel that should have shown it.
		if (!mainOpen && mainWorld == world) {
			return 0;
		}

		// A panel that is closed, or open and following the active scene rather
		// than pinned to one. Either is free: nobody has said it must show
		// something else.
		for (size_t index = 0; index < panels.size(); index++) {
			if (!panels[index].Open || !panels[index].World.IsValid()) {
				return index + 1;
			}
		}

		return NO_VIEWPORT;
	}
}
