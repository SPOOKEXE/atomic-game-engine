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
#include <engine/scene/Storm.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <array>
#include <cstddef>
#include <optional>

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
		// Combines borrowed spatial and screen overlay recorders.
		WorldViewInterface(FrameOverlayHook *spatial, FrameOverlayHook *screen);
		bool Prepare(void *commandBuffer) override;
		bool SupportsWorldLayers() const override;
		// Reports whether the spatial hook has world-space content.
		bool HasWorldOverlay() const override;
		// Returns the number of spatial batches ready to record.
		size_t WorldBatchCount() const override;
		// Records one spatial interface batch into the active pass.
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

		// Published name of the source world.
		core::Name Name;
		// Live store identity that authorizes this packet.
		uint64_t Identity = 0;
		// Simulation tick represented by this packet.
		uint64_t Tick = 0;
		// Elapsed world time at Tick, in seconds.
		double Seconds = 0;
		// Fixed simulation duration represented by this packet. The particle
		// device step consumes it once when this revision reaches the renderer.
		float ParticleDelta = 0.0f;
		// Camera-independent lighting copied from the world.
		scene::WorldLighting Lighting;
		// Packed cloud-density snapshot copied from the storm field.
		std::optional<scene::CloudDensitySnapshot> CloudDensity;
		// Field inputs used to invalidate the cached density when a preset changes.
		std::optional<scene::TornadoParameters> CloudParameters;
		// Last density rebuild in world time; the volume moves every tick while
		// its expensive local density pattern changes only four times a second.
		double CloudBuiltSeconds = -1.0;
		// Renderable instances in presentation order.
		std::vector<scene::DrawInstance> Instances;
		// Object labels for capture output.
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		// Semantic labels for capture output.
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		// Part labels for capture output.
		std::vector<DataCapturePartLabel> PartLabels;
		// Whether ObjectLabels was produced without overflow.
		bool ObjectLabelsValid = true;
		// Whether SemanticLabels was produced without overflow.
		bool SemanticLabelsValid = true;
		// Whether PartLabels was produced without overflow.
		bool PartLabelsValid = true;
		// Skin joint frames referenced by skinned instances.
		std::vector<core::CFrame> Joints;
		// Surface slots requested by the world.
		std::vector<scene::SurfaceSlot> Slots;
		// Portal seams visible in this presentation packet.
		std::vector<scene::PortalSeam> Seams;
		// Portal views collected from the world.
		std::vector<PortalView> Portals;
		// Detached particle data owned by this packet.
		ParticleFrame Particles;
		// Device-local analytical particles. This is a copied request and field,
		// never a borrowed ECS row.
		std::optional<GpuParticleFieldView> GpuParticles;
	};

	// One camera's light selection, surface demand, facing ribbons and spatial
	// interface commands.
	struct WorldCameraFrame {
		// Camera-local selection of the world's bounded participating-media set.
		std::array<scene::VolumeState, scene::MAX_SCENE_VOLUMES> Volumes{};
		// Number of selected volumes in `Volumes`.
		size_t VolumeCount = 0;
		// Lights selected for this camera.
		std::vector<SceneLight> Lights;
		// Surface views demanded by this camera.
		std::vector<SurfaceView> Surfaces;
		// Camera-facing ribbon geometry.
		effects::RibbonBuffer Ribbons;
		// Compiled game interface for this camera.
		gui::Compiled Compiled;
		// World-space interface commands.
		gui::DrawList SpatialCommands;
		// Collectors that own spatial interface resources.
		std::vector<SpatialCollector> SpatialCollectors;
	};

	// The live presentation owner authorizes a retained packet by name and store identity.
	struct WorldViewBinding {
		// Live store identity that authorizes the binding.
		uint64_t World = 0;
		// Published name of the bound world.
		core::Name Name;
		// Store incarnation expected by the binding.
		uint64_t Identity = 0;
		// Content namespace used for local asset resolution.
		core::Name ContentOwner;
		// Additional content namespaces visible to the view.
		std::span<const WorldContentOwner> ForeignContentOwners;
		// Render pipeline selected for this view.
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
	// A frozen capture compiles camera-dependent GUI in a copied store so its
	// retained source snapshot stays byte-identical.
	void CollectWorldCamera(
		ecs::Store &store,
		const View &view,
		const core::Vector2 &extent,
		WorldCameraFrame &frame,
		bool frozen = false
	);

}
