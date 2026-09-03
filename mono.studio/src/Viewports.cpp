#include <engine/ecs/Store.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <studio/Viewports.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::Vector3;
	using engine::world::WorldId;

	ViewportTargetSize ResolveViewportTargetSize(
		uint32_t panelWidth,
		uint32_t panelHeight,
		uint32_t imageWidth,
		uint32_t imageHeight,
		uint32_t maximumWidth,
		uint32_t maximumHeight
	) {
		const bool explicitImage = imageWidth > 0 && imageHeight > 0;
		const uint32_t sourceWidth = std::max(explicitImage ? imageWidth : panelWidth, 1u);
		const uint32_t sourceHeight = std::max(explicitImage ? imageHeight : panelHeight, 1u);

		double scale = 1.0;
		if (maximumWidth > 0) {
			scale = std::min(scale, static_cast<double>(maximumWidth) / static_cast<double>(sourceWidth));
		}
		if (maximumHeight > 0) {
			scale = std::min(scale, static_cast<double>(maximumHeight) / static_cast<double>(sourceHeight));
		}

		return ViewportTargetSize{
			std::max(static_cast<uint32_t>(std::llround(static_cast<double>(sourceWidth) * scale)), 1u),
			std::max(static_cast<uint32_t>(std::llround(static_cast<double>(sourceHeight) * scale)), 1u),
		};
	}

	ViewportCameraPose DefaultViewportCamera() {
		ViewportCameraPose pose;
		pose.Frame = CFrame::LookAt(Vector3{0.0f, 30.0f, 30.0f}, Vector3::Zero);
		const Vector3 angles = pose.Frame.ToAngles();
		pose.Pitch = angles.X;
		pose.Yaw = angles.Y;
		return pose;
	}

	bool CarryViewportCamera(engine::ecs::Store &store, const CFrame &previous, ViewportCameraPose &pose) {
		if (previous.Position == pose.Frame.Position) {
			return false;
		}

		engine::scene::SeamTransform through;
		if (!engine::scene::PortalCrossing(store, previous.Position, pose.Frame.Position, through)) {
			return false;
		}

		pose.Frame = through.Place(pose.Frame);
		Vector3 position = pose.Frame.Position;
		(void)engine::scene::ClearOfPanes(store, position);
		pose.Frame.Position = position;

		const Vector3 angles = pose.Frame.ToAngles();
		pose.Pitch = angles.X;
		pose.Yaw = angles.Y;
		return true;
	}

	Vector3
	CameraRelativeMovement(const CFrame &rotation, const float forward, const float right, const float up) {
		return rotation.LookVector() * forward + rotation.RightVector() * right + rotation.UpVector() * up;
	}

	CFrame SnapViewportCameraDirection(const CFrame &frame, const Vector3 &direction) {
		const float alignment = frame.LookVector().Dot(direction);
		constexpr float ALIGNED = 0.9999f;
		if (alignment > ALIGNED || alignment < -ALIGNED) {
			const Vector3 inverse = direction * (alignment > 0.0f ? -1.0f : 1.0f);
			return CFrame::LookAt(frame.Position, frame.Position + inverse, frame.UpVector());
		}

		return CFrame::LookAt(frame.Position, frame.Position + direction);
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
