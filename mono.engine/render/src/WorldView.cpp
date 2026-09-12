#include "EnvironmentModes.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scene/Sunlight.hpp>

#include <algorithm>

namespace engine::render {
	bool PrepareWorldShaders(
		ecs::Store &store,
		core::Name owner,
		ShaderLibrary &library,
		Renderer &renderer,
		InterfacePass *interface,
		bool postProcessing
	) {
		ENGINE_PROFILE_CAT("world shaders prepare", core::ProfileCategory::Render);
		library.Refresh(store, owner);
		library.RefreshLenses(store, owner);
		for (const auto name : library.Changed()) {
			const auto *module = library.Find(name, owner);
			if (module != nullptr && !module->Error.empty())
				ENGINE_WARN("shader '{}': {}", name.Text(), module->Error);
		}
		for (const auto name : library.ChangedLenses()) {
			const auto *module = library.FindLens(name, owner);
			if (module != nullptr && !module->Error.empty())
				ENGINE_WARN("lens shader '{}': {}", name.Text(), module->Error);
		}
		std::vector<core::Name> materials, lenses, pictures;
		store.Each<const scene::MaterialRef>([&](ecs::Entity, const scene::MaterialRef &material) {
			if (material.Shader.IsValid()) materials.push_back(material.Shader);
		});
		std::sort(materials.begin(), materials.end(), [](auto left, auto right) {
			return left.Id() < right.Id();
		});
		materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
		scene::DemandedLensShaders(store, lenses);
		const auto grade = postProcessing ? scene::PostProcessShaderOf(store) : core::Name{};
		const bool deviceChanged = renderer.PrepareShaders(library, materials, lenses, grade, owner);
		if (interface == nullptr) return deviceChanged;
		gui::DemandedShaders(store, pictures);
		const bool ownerChanged = interface->SetContentOwner(owner);
		return interface->RefreshShaders(pictures, library, owner) > 0 || deviceChanged || ownerChanged;
	}

	WorldViewInterface::WorldViewInterface(FrameOverlayHook *spatial, FrameOverlayHook *screen)
		: Spatial(spatial), Screen(screen) {}

	bool WorldViewInterface::Prepare(void *commandBuffer) {
		SpatialReady = Spatial && Spatial->Prepare(commandBuffer);
		ScreenReady = Screen == Spatial ? SpatialReady : Screen && Screen->Prepare(commandBuffer);
		return SpatialReady || ScreenReady;
	}

	bool WorldViewInterface::SupportsWorldLayers() const {
		return !Spatial || Spatial->SupportsWorldLayers();
	}
	bool WorldViewInterface::HasWorldOverlay() const {
		return Spatial && Spatial->HasWorldOverlay();
	}

	size_t WorldViewInterface::WorldBatchCount() const {
		return SpatialReady ? Spatial->WorldBatchCount() : 0;
	}

	uint32_t WorldViewInterface::RecordWorldBatch(const WorldInterfaceCapture &capture, size_t batch) {
		return SpatialReady ? Spatial->RecordWorldBatch(capture, batch) : 0;
	}

	uint32_t WorldViewInterface::RecordWorld(
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
	) {
		return SpatialReady
				   ? Spatial->RecordWorld(
						 commandBuffer, pass, projection, camera, ambient, sun, width, height, depth, target
					 )
				   : 0;
	}

	bool WorldViewInterface::AffectsScene() const {
		return Spatial && Spatial->AffectsScene();
	}

	void WorldViewInterface::Record(void *commandBuffer, void *pass) {
		if (ScreenReady) Screen->Record(commandBuffer, pass);
	}

	bool BindWorldView(
		const WorldViewFrame &frame,
		const WorldCameraFrame &camera,
		const WorldViewBinding &binding,
		View &view
	) {
		if (!binding.Name.IsValid() || frame.Name != binding.Name || binding.Identity == 0 ||
			frame.Identity != binding.Identity)
			return false;
		view.World = binding.World;
		view.WorldName = frame.Name;
		view.ContentOwner = binding.ContentOwner;
		view.ForeignContentOwners = binding.ForeignContentOwners;
		view.Pipeline = binding.Pipeline;
		view.Instances = frame.Instances;
		view.JointFrames = frame.Joints;
		view.Lighting = frame.Lighting;
		view.OverrideLighting = true;
		view.Lights = camera.Lights;
		view.Surfaces = camera.Surfaces;
		view.RibbonVertices = camera.Ribbons.Vertices;
		view.RibbonRuns = camera.Ribbons.Runs;
		view.Particles = frame.Particles.Batches;
		view.ParticleSeams = frame.Particles.Seams;
		view.ParticleRevision = frame.Particles.Revision;
		view.ParticleLayoutRevision = frame.Particles.LayoutRevision;
		view.ParticleResidentRevision = frame.Particles.ResidentRevision;
		view.ParticlePool = frame.Particles.Pool;
		view.ParticleBlocks = frame.Particles.BlockCount;
		view.Portals = frame.Portals;
		view.EyeImage = 0;
		view.EyeTransparentImages = {};
		view.EyeSpatialOverlayImage = 0;
		view.EyeImageKey = {};
		view.LensPrograms = 0;
		view.LensContentOwner.reset();
		view.LensTimeSeconds = static_cast<float>(frame.Seconds);
		return true;
	}

	void CollectWorldView(ecs::Store &store, core::Name owner, WorldViewFrame &frame) {
		ENGINE_PROFILE_CAT("world view collect", core::ProfileCategory::Render);
		frame.Name = owner;
		frame.Identity = store.Identity();
		frame.Tick = store.Time().Tick;
		frame.Seconds = store.Time().Elapsed;
		frame.Lighting = scene::LightingOf(store);
		// An inactive cloud clock cannot change the captured pixels.
		if (EnvironmentModesOf(frame.Lighting.EnvironmentState).Clouds == 0 ||
			frame.Lighting.EnvironmentState.CloudLayer.WindSpeed <= 0)
			frame.Lighting.EnvironmentState.CloudTime = 0;
		frame.Instances.clear();
		frame.Joints.clear();
		if (const auto *draw = store.Resource<DrawList>()) {
			frame.Instances = draw->Instances;
			frame.Joints = draw->JointFrames;
		}
		scene::GatherSurfaceSlots(store, frame.Slots);
		ApplySurfaceSlots(frame.Instances, frame.Slots, owner);
		scene::GatherPortalSeams(store, frame.Seams);
		for (auto &seam : frame.Seams) {
			for (const auto &slot : frame.Slots)
				if (slot.Camera == seam.Camera) seam.Surface = slot.Index;
		}
		CollectPortalViews(store, frame.Portals, frame.Slots);
		CollectParticleBatches(store, frame.Particles);
		frame.Particles.Detach();
	}

	void CollectWorldCamera(
		ecs::Store &store, const View &view, const core::Vector2 &extent, WorldCameraFrame &frame
	) {
		ENGINE_PROFILE_CAT("world camera collect", core::ProfileCategory::Render);
		CollectLights(store, view.CameraFrame.Position, frame.Lights);
		CollectSurfaceViews(store, frame.Surfaces, view.Portals, &view);
		effects::BuildRibbons(store, view.CameraFrame.Position, float(store.Time().Elapsed), frame.Ribbons);
		const gui::Screen screen{extent.X, extent.Y};
		ResolveSpatialCanvases(store, screen, &view.Camera, &view.CameraFrame);
		gui::CompileRequest compile;
		compile.Display = screen;
		compile.Seconds = store.Time().Elapsed;
		frame.Compiled.Rebuild(store, compile);
		frame.SpatialCommands = frame.Compiled.Commands();
		std::erase_if(frame.SpatialCommands.Commands, [&](const gui::DrawCommand &command) {
			return store.Get<gui::SpatialCanvas>(command.Collector) == nullptr;
		});
		frame.SpatialCollectors.clear();
		store.Each<const gui::SpatialCanvas>([&](ecs::Entity collector, const gui::SpatialCanvas &canvas) {
			frame.SpatialCollectors.push_back({collector, canvas});
		});
	}
}
