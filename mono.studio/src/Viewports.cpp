#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <studio/Viewports.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::Vector3;
	using engine::world::WorldId;

	engine::gui::Screen
	ResolveGuiPreviewScreen(GuiPreviewSettings settings, float panelWidth, float panelHeight) {
		engine::gui::Screen screen;
		switch (settings.Profile) {
		case GuiPreviewProfile::Desktop:
			screen.Width = panelWidth;
			screen.Height = panelHeight;
			break;
		case GuiPreviewProfile::Phone:
			screen.Width = 390.0f;
			screen.Height = 844.0f;
			screen.SafeArea.Top = 47.0f;
			screen.SafeArea.Bottom = 34.0f;
			break;
		case GuiPreviewProfile::Tablet:
			screen.Width = 1024.0f;
			screen.Height = 1366.0f;
			screen.SafeArea.Top = 24.0f;
			screen.SafeArea.Bottom = 20.0f;
			break;
		}
		screen.InterfaceScale = std::max(0.1f, settings.InterfaceScale);
		screen.TextScale = std::max(0.1f, settings.TextScale);
		return screen;
	}

	glm::vec2 GuiPreviewControlsPosition(glm::vec2 imageMinimum) {
		return imageMinimum + glm::vec2{8.0f, 8.0f};
	}

	bool ViewportDirectionControls::WireframeContains(float x, float y) const {
		return x >= WireframeLeft && x <= WireframeRight && y >= WireframeTop && y <= WireframeBottom;
	}

	ViewportDirectionControls
	ResolveViewportDirectionControls(float panelX, float panelY, float panelWidth, float interfaceScale) {
		const float scale = std::max(interfaceScale, 0.01f);
		ViewportDirectionControls controls;
		controls.Radius = 28.0f * scale;
		controls.GimbalPadding = 18.0f * scale;
		controls.CentreX = panelX + panelWidth - controls.Radius - 20.0f * scale;
		controls.CentreY = panelY + controls.Radius + 20.0f * scale;

		const float buttonWidth = 88.0f * scale;
		const float buttonHeight = 24.0f * scale;
		controls.WireframeLeft = controls.CentreX - buttonWidth * 0.5f;
		controls.WireframeTop = controls.CentreY + controls.Radius + controls.GimbalPadding + 6.0f * scale;
		controls.WireframeRight = controls.WireframeLeft + buttonWidth;
		controls.WireframeBottom = controls.WireframeTop + buttonHeight;
		return controls;
	}

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

	engine::ecs::Entity
	CreateRuntimeCamera(engine::ecs::Store &store, std::string_view name, const ViewportCameraPose &pose) {
		const engine::ecs::Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace == engine::ecs::NULL_ENTITY) {
			return engine::ecs::NULL_ENTITY;
		}

		const engine::ecs::Entity camera = store.CreateInstance(engine::scene::CameraClass(), name);
		if (camera == engine::ecs::NULL_ENTITY) {
			return camera;
		}

		store.SetParent(camera, workspace);
		store.Set(camera, engine::scene::TransientComponent{});
		store.Set(camera, engine::scene::Transform{pose.Frame});

		engine::scene::ActiveCamera active;
		if (const auto *existing = store.Resource<engine::scene::ActiveCamera>(); existing != nullptr) {
			active = *existing;
		}
		active.Entity = camera;
		store.SetResource(active);
		return camera;
	}

	engine::ecs::Entity RuntimeCameraOf(const engine::ecs::Store &store) {
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		if (active == nullptr || active->Entity == engine::ecs::NULL_ENTITY || !store.Alive(active->Entity)) {
			return engine::ecs::NULL_ENTITY;
		}
		if (store.Get<engine::scene::Camera>(active->Entity) == nullptr ||
			store.Get<engine::scene::Transform>(active->Entity) == nullptr) {
			return engine::ecs::NULL_ENTITY;
		}
		return active->Entity;
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

	ViewportGuiSource ViewportGuiSourceFor(bool running, bool clientView) {
		if (clientView) {
			return ViewportGuiSource::PlayerGui;
		}
		return running ? ViewportGuiSource::None : ViewportGuiSource::StarterGui;
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
