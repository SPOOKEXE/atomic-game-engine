#pragma once

// Owned presentation packets for cameras that do not own a world's active camera.
// Collect the world once after PreRender, then sample each ready camera's layers.
// These copies stay on the presentation thread and never change simulation state.
//
// @tier L12 · client

#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/SurfaceCameras.hpp>

namespace engine::render {
	class ShaderLibrary;
	class InterfacePass;

	// Prepares one world's demanded programs on the render owner thread.
	// The library may be shared by every view using this renderer. Returns damage.
	bool PrepareWorldShaders(
		ecs::Store &store,
		core::Name owner,
		ShaderLibrary &library,
		Renderer &renderer,
		InterfacePass *interface = nullptr,
		bool postProcessing = true
	);

	// Routes world-space UI to the displayed world and screen UI to the player.
	// Both hooks borrow their resources for the duration of one render call.
	class WorldViewInterface : public FrameOverlayHook {
	  public:
		WorldViewInterface(FrameOverlayHook *spatial, FrameOverlayHook *screen);
		bool Prepare(void *commandBuffer) override;
		bool SupportsWorldLayers() const override;
		bool HasWorldOverlay() const override;
		size_t WorldBatchCount() const override;
		uint32_t RecordWorldBatch(const WorldInterfaceCapture &capture, size_t batch) override;
		uint32_t RecordWorld(
			void *commandBuffer,
			void *pass,
			const glm::mat4 &projection,
			const core::CFrame &camera,
			const core::Color3 &ambient,
			const core::Vector3 &sun,
			uint32_t width,
			uint32_t height,
			bool depth,
			WorldColourTarget target
		) override;
		bool AffectsScene() const override;
		void Record(void *commandBuffer, void *pass) override;

	  private:
		FrameOverlayHook *Spatial = nullptr;
		FrameOverlayHook *Screen = nullptr;
		bool SpatialReady = false;
		bool ScreenReady = false;
	};

	// One world's published pose and camera-independent rendering inputs.
	// Noncopyable because particle batches point into the owned detached arrays.
	struct WorldViewFrame {
		WorldViewFrame() = default;
		WorldViewFrame(const WorldViewFrame &) = delete;
		WorldViewFrame &operator=(const WorldViewFrame &) = delete;

		core::Name Name;
		uint64_t Identity = 0;
		uint64_t Tick = 0;
		double Seconds = 0;
		scene::WorldLighting Lighting;
		std::vector<scene::DrawInstance> Instances;
		std::vector<core::CFrame> Joints;
		std::vector<scene::SurfaceSlot> Slots;
		std::vector<scene::PortalSeam> Seams;
		std::vector<PortalView> Portals;
		ParticleFrame Particles;
	};

	// One camera's light selection, surface demand, facing ribbons and spatial
	// interface commands.
	struct WorldCameraFrame {
		std::vector<SceneLight> Lights;
		std::vector<SurfaceView> Surfaces;
		effects::RibbonBuffer Ribbons;
		gui::Compiled Compiled;
		gui::DrawList SpatialCommands;
		std::vector<SpatialCollector> SpatialCollectors;
	};

	// The live presentation owner authorizes a retained packet by name and store identity.
	struct WorldViewBinding {
		uint64_t World = 0;
		core::Name Name;
		uint64_t Identity = 0;
		core::Name ContentOwner;
		std::span<const WorldContentOwner> ForeignContentOwners;
		core::Name Pipeline;
	};

	// Borrows a complete world packet at the caller's current camera, target and slot.
	// Camera layers must be collected for that camera from the same live store.
	// Refusal leaves the view unchanged. Packets and owner bindings outlive the render.
	// Body selection, surface requests and presentation damage remain caller-owned.
	bool BindWorldView(
		const WorldViewFrame &frame,
		const WorldCameraFrame &camera,
		const WorldViewBinding &binding,
		View &view
	);

	// Copies the published pose and palette, preserving replica interpolation.
	// Does not run presentation or rebuild transforms. Missing DrawList yields an
	// empty geometry layer. All particle pointers are detached before returning.
	void CollectWorldView(ecs::Store &store, core::Name owner, WorldViewFrame &frame);

	// Uses the supplied camera and target size without replacing ActiveCamera.
	// Only spatial collectors enter the packet; the player's screen UI has its
	// own owner. The world must already have completed its presentation phase.
	void CollectWorldCamera(
		ecs::Store &store, const View &view, const core::Vector2 &extent, WorldCameraFrame &frame
	);

}
