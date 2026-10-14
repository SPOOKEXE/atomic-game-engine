#include "EnvironmentModes.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/gui/Components.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine::render {
	namespace {
		void RetireWeatherCheckpoint(WorldViewFrame &frame) {
			if (!frame.WeatherCheckpoint.empty()) {
				std::vector<std::byte>{}.swap(frame.WeatherCheckpoint);
				frame.WeatherCheckpointVersion =
					frame.WeatherCheckpointVersion == UINT64_MAX ? 1 : frame.WeatherCheckpointVersion + 1;
			}
			frame.WeatherCheckpointTick = 0;
			frame.WeatherCheckpointEpoch = 0;
			frame.WeatherCheckpointRevision = 0;
			frame.WeatherCheckpointSource = ecs::NULL_ENTITY;
		}

		void CollectWeatherSimulationLayer(ecs::Store &store, WorldViewFrame &frame, bool newWorld) {
			const auto &environment = frame.Lighting.EnvironmentState;
			frame.WeatherTerrainRevision = environment.HasWeatherCompute && environment.Weather.Enabled
											   ? environment.Weather.TerrainRevision
											   : 0;
			if (!environment.HasWeatherCompute || !environment.Weather.Enabled ||
				environment.CloudProvider == ecs::NULL_ENTITY) {
				frame.WeatherSimulationLayer = {};
				frame.WeatherSimulationLayerSource = ecs::NULL_ENTITY;
				return;
			}
			store.Observe<ecs::InstanceName>();
			store.Observe<ecs::Hierarchy>();
			store.Observe<scene::WeatherCompute>();
			store.Observe<scene::WeatherAcceptedDefinition>();
			const auto nameWrites = store.ComponentChangeVersion<ecs::InstanceName>();
			const auto hierarchyWrites = store.ComponentChangeVersion<ecs::Hierarchy>();
			const auto definitionWrites = store.ComponentChangeVersion<scene::WeatherCompute>();
			const auto acceptedWrites = store.ComponentChangeVersion<scene::WeatherAcceptedDefinition>();
			const auto acceptedMembership =
				store.ComponentMembershipVersion<scene::WeatherAcceptedDefinition>();
			const bool changed = newWorld ||
								 frame.WeatherSimulationLayerSource != environment.CloudProvider ||
								 frame.WeatherLayerNameWrites != nameWrites ||
								 frame.WeatherLayerHierarchyWrites != hierarchyWrites ||
								 frame.WeatherLayerDefinitionWrites != definitionWrites ||
								 frame.WeatherLayerAcceptedWrites != acceptedWrites ||
								 frame.WeatherLayerAcceptedMembership != acceptedMembership;
			// Accepted identity survives pending renames. Prefer the existing ECS-owned
			// string so even a changed world need not allocate another temporary path.
			std::string_view canonical;
			if (const auto *accepted = store.Get<scene::WeatherAcceptedDefinition>(environment.CloudProvider))
				canonical = accepted->Layer;
			if (canonical.empty()) {
				if (const auto *cache = store.Resource<weather::WeatherRuntimeCache>())
					for (const auto &layer : cache->Layers)
						if (layer.Provider == environment.CloudProvider && !layer.AmbiguousIdentity) {
							canonical = layer.Layer;
							break;
						}
			}
			if (!canonical.empty()) {
				if (canonical.size() > scene::MAX_WEATHER_STREAM_LAYER_BYTES)
					frame.WeatherSimulationLayer = {};
				else if (frame.WeatherSimulationLayer.Text() != canonical)
					frame.WeatherSimulationLayer = core::Name(canonical);
			} else if (changed) {
				const auto layer = weather::WeatherLayerName(store, environment.CloudProvider);
				frame.WeatherSimulationLayer = layer.empty() ? core::Name{} : core::Name(layer);
			}
			frame.WeatherSimulationLayerSource = environment.CloudProvider;
			frame.WeatherLayerNameWrites = nameWrites;
			frame.WeatherLayerHierarchyWrites = hierarchyWrites;
			frame.WeatherLayerDefinitionWrites = definitionWrites;
			frame.WeatherLayerAcceptedWrites = acceptedWrites;
			frame.WeatherLayerAcceptedMembership = acceptedMembership;
		}

		void CollectWeatherCheckpoint(ecs::Store &store, WorldViewFrame &frame, bool newWorld) {
			const auto &environment = frame.Lighting.EnvironmentState;
			store.Observe<weather::StatefulWeatherCheckpoint>();
			const auto changeVersion = store.ComponentChangeVersion<weather::StatefulWeatherCheckpoint>();
			const bool medium =
				environment.CloudLayer.Cover > 0 && environment.CloudLayer.Density > 0 &&
				(environment.CloudLayer.Enabled ||
				 (environment.Weather.ShadowsEnabled && environment.Weather.ShadowStrength > 0 &&
				  environment.Weather.ShadowDistanceMetres > 0));
			const auto *checkpoint =
				environment.HasWeatherCompute && environment.Weather.Enabled && environment.HasClouds &&
						medium && scene::ValidWeatherCompute(environment.Weather) &&
						environment.Weather.Mode == scene::WeatherMode::Stateful
					? store.Get<weather::StatefulWeatherCheckpoint>(environment.CloudProvider)
					: nullptr;
			if (!checkpoint || checkpoint->Bytes.empty() ||
				checkpoint->Bytes.size() > weather::CheckpointLimits{}.MaximumBytes ||
				checkpoint->Epoch == 0 || checkpoint->Revision != environment.Weather.DefinitionRevision ||
				(environment.HasWeatherClock && checkpoint->Epoch != environment.Clock.Epoch)) {
				RetireWeatherCheckpoint(frame);
				frame.WeatherCheckpointChangeVersion = changeVersion;
				return;
			}
			const bool replaced = newWorld || frame.WeatherCheckpointSource != environment.CloudProvider ||
								  frame.WeatherCheckpointTick != checkpoint->Tick ||
								  frame.WeatherCheckpointEpoch != checkpoint->Epoch ||
								  frame.WeatherCheckpointRevision != checkpoint->Revision;
			const bool changed = changeVersion != frame.WeatherCheckpointChangeVersion;
			// Dirty rows identify same-metadata edits until ClearChanges. Past that
			// watermark, a missed write requires a conservative refresh of this one provider.
			const bool selectedChanged =
				changed && (store.Changed<weather::StatefulWeatherCheckpoint>(environment.CloudProvider) ||
							frame.WeatherCheckpointChangeVersion <
								store.ComponentClearedChangeVersion<weather::StatefulWeatherCheckpoint>());
			if (replaced || frame.WeatherCheckpoint.empty() ||
				(selectedChanged && frame.WeatherCheckpoint != checkpoint->Bytes)) {
				ENGINE_PROFILE_CAT("weather checkpoint collect", core::ProfileCategory::Render);
				frame.WeatherCheckpoint = checkpoint->Bytes;
				frame.WeatherCheckpointSource = environment.CloudProvider;
				frame.WeatherCheckpointTick = checkpoint->Tick;
				frame.WeatherCheckpointEpoch = checkpoint->Epoch;
				frame.WeatherCheckpointRevision = checkpoint->Revision;
				frame.WeatherCheckpointVersion =
					frame.WeatherCheckpointVersion == UINT64_MAX ? 1 : frame.WeatherCheckpointVersion + 1;
				core::Metrics::Count("render.weather.checkpoint_collect_bytes", checkpoint->Bytes.size());
				core::Metrics::Count("render.weather.checkpoint_collect_copies", 1);
			}
			frame.WeatherCheckpointChangeVersion = changeVersion;
		}
	}

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
		view.ObjectLabels = frame.ObjectLabels;
		view.SemanticLabels = frame.SemanticLabels;
		view.PartLabels = frame.PartLabels;
		view.ObjectLabelsValid = frame.ObjectLabelsValid;
		view.SemanticLabelsValid = frame.SemanticLabelsValid;
		view.PartLabelsValid = frame.PartLabelsValid;
		view.JointFrames = frame.Joints;
		view.Lighting = frame.Lighting;
		view.CloudDensity = frame.CloudDensity;
		view.WeatherCheckpoint = frame.WeatherCheckpoint;
		view.WeatherSimulationLayer = frame.WeatherSimulationLayer;
		view.WeatherTerrainRevision = frame.WeatherTerrainRevision;
		view.WeatherCheckpointVersion = frame.WeatherCheckpointVersion;
		view.WeatherCheckpointTick = frame.WeatherCheckpointTick;
		view.WeatherCheckpointEpoch = frame.WeatherCheckpointEpoch;
		view.WeatherCheckpointRevision = frame.WeatherCheckpointRevision;
		view.Lighting.Volumes = camera.Volumes;
		view.Lighting.VolumeCount = camera.VolumeCount;
		if (camera.WeatherEffects.HasLighting) view.Lighting = camera.WeatherEffects.Lighting;
		view.WeatherParticles =
			camera.WeatherEffects.Precipitation
				? std::span<const GpuParticleFieldView>(&*camera.WeatherEffects.Precipitation, 1)
				: std::span<const GpuParticleFieldView>{};
		view.OverrideLighting = true;
		view.Lights = camera.Lights;
		view.Surfaces = camera.Surfaces;
		view.RibbonVertices = camera.Ribbons.Vertices;
		view.RibbonRuns = camera.Ribbons.Runs;
		view.Particles = frame.Particles.Batches;
		view.ParticleSeams = frame.Particles.Seams;
		view.ParticleWorld = binding.World;
		view.ParticleWorldName = frame.Name;
		view.ParticleRevision = frame.Particles.Revision;
		view.ParticleLayoutRevision = frame.Particles.LayoutRevision;
		view.ParticleResidentRevision = frame.Particles.ResidentRevision;
		view.ParticlePool = frame.Particles.Pool;
		view.ParticleDelta = frame.ParticleDelta;
		view.ParticleBlocks = frame.Particles.BlockCount;
		view.GpuParticles = frame.GpuParticles;
		if (view.GpuParticles)
			core::Metrics::Count(
				"render.gpu_particle_field.sample_bind_bytes",
				view.GpuParticles->Field.SpawnSamples.size() * sizeof(scene::GpuParticleSpawnSample)
			);
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
		const ecs::WorldTime time = store.Time();
		// A collected packet can be rebound for another camera while its source
		// world is paused. Only a new source or world tick carries time to the device pool.
		const bool newWorld = frame.Name != owner || frame.Identity != store.Identity();
		const bool advanced = newWorld || time.Tick != frame.Tick;
		frame.Name = owner;
		frame.Identity = store.Identity();
		frame.Tick = time.Tick;
		frame.Seconds = time.Elapsed;
		frame.ParticleDelta = advanced ? time.Delta : 0.0f;
		bool fieldCollected = false;
		store.Observe<scene::GpuParticleField>();
		const uint64_t fieldDefinitionRevision = store.ComponentChangeVersion<scene::GpuParticleField>() + 1;
		store.Each<const scene::GpuParticleField, const scene::Transform>(
			[&](ecs::Entity instance,
				const scene::GpuParticleField &field,
				const scene::Transform &transform) {
				if (!fieldCollected && field.Enabled && !field.SpawnSamples.empty() &&
					scene::ValidGpuParticleField(field)) {
					// Refresh the owned packet in place so its sample capacity survives each tick.
					if (!frame.GpuParticles) frame.GpuParticles.emplace();
					auto &packet = *frame.GpuParticles;
					if (newWorld || packet.Source != instance ||
						packet.DefinitionRevision != fieldDefinitionRevision) {
						packet.Field = field;
						core::Metrics::Count(
							"render.gpu_particle_field.sample_collect_bytes",
							field.SpawnSamples.size() * sizeof(scene::GpuParticleSpawnSample)
						);
					}
					packet.Frame = transform.Frame;
					packet.ForceField = scene::ResolveVectorField(store, instance);
					packet.Source = instance;
					packet.DefinitionRevision = fieldDefinitionRevision;
					packet.Seconds = time.Elapsed;
					fieldCollected = true;
				}
			}
		);
		if (!fieldCollected) frame.GpuParticles.reset();

		frame.Lighting = scene::LightingOf(store);
		CollectWeatherSimulationLayer(store, frame, newWorld);
		CollectWeatherCheckpoint(store, frame, newWorld);
		if (newWorld) frame.CloudDensity.reset();

		// An inactive cloud clock cannot change the captured pixels.
		if (EnvironmentModesOf(frame.Lighting.EnvironmentState).Clouds == 0 ||
			frame.Lighting.EnvironmentState.CloudLayer.WindSpeed <= 0)
			frame.Lighting.EnvironmentState.CloudTime = 0;
		frame.Instances.clear();
		frame.Joints.clear();
		if (const auto *draw = store.Resource<DrawList>()) {
			frame.Instances = draw->Instances;
			frame.ObjectLabels = draw->ObjectLabels;
			frame.SemanticLabels = draw->SemanticLabels;
			frame.PartLabels = draw->PartLabels;
			frame.ObjectLabelsValid = draw->ObjectLabelsValid;
			frame.SemanticLabelsValid = draw->SemanticLabelsValid;
			frame.PartLabelsValid = draw->PartLabelsValid;
			frame.Joints = draw->JointFrames;
		}
		scene::GatherSurfaceSlots(store, frame.Slots);
		scene::GatherPortalSeams(store, frame.Seams);
		for (auto &seam : frame.Seams) {
			for (const auto &slot : frame.Slots)
				if (slot.Camera == seam.Camera) seam.Surface = slot.Index;
		}
		ApplySurfaceSlots(frame.Instances, frame.Slots, owner, frame.Seams);
		CollectPortalViews(store, frame.Portals, frame.Slots);
		CollectParticleBatches(store, frame.Particles);
		frame.Particles.Detach();
	}

	void CollectWorldCamera(
		ecs::Store &store, const View &view, const core::Vector2 &extent, WorldCameraFrame &frame, bool frozen
	) {
		ENGINE_PROFILE_CAT("world camera collect", core::ProfileCategory::Render);
		const core::CFrame &visibilityFrame = view.VisibilityCameraFrame();
		static thread_local std::vector<uint32_t> visible;
		static thread_local std::vector<core::AABB> receivers;
		std::optional<graph::Frustum> frustum;
		receivers.clear();
		if (extent.X > 0.0f && extent.Y > 0.0f) {
			const scene::CameraMatrices matrices =
				view.Projection ? scene::ResolveSurfaceCamera(visibilityFrame, *view.Projection)
								: scene::ResolveCamera(visibilityFrame, view.Camera, extent.X / extent.Y);
			frustum = graph::Frustum::FromViewProjection(matrices.ViewProjection);
			graph::Cull(view.Instances, *frustum, visible);
			receivers.reserve(visible.size());
			for (const uint32_t index : visible) {
				receivers.push_back(graph::BoundsOf(view.Instances[index]));
			}
		}
		CollectLights(store, visibilityFrame.Position, receivers, frame.Lights);
		frame.VolumeCount = scene::ResolveVolumes(store, visibilityFrame.Position, receivers, frame.Volumes);
		View visibilityView = view;
		visibilityView.CameraFrame = visibilityFrame;
		visibilityView.VisibilityFrame.reset();
		CollectSurfaceViews(store, frame.Surfaces, view.Portals, &visibilityView);
		effects::BuildRibbons(store, visibilityFrame.Position, float(store.Time().Elapsed), frame.Ribbons);
		View weatherView = view;
		weatherView.Lighting.Volumes = frame.Volumes;
		weatherView.Lighting.VolumeCount = frame.VolumeCount;
		CollectWeatherEffects(store, weatherView, frame.WeatherEffects, frame.Ribbons, frame.Lights);
		gui::Screen screen;
		screen.Width = extent.X;
		screen.Height = extent.Y;
		std::optional<ecs::Store> frozenGui;
		ecs::Store *guiStore = &store;
		if (frozen) {
			frozenGui.emplace("capture-spatial-gui");
			if (!store.CloneTo(*frozenGui)) {
				frame.SpatialCommands = {};
				frame.SpatialCollectors.clear();
				return;
			}
			guiStore = &*frozenGui;
		}
		ResolveSpatialCanvases(*guiStore, screen, &view.Camera, &visibilityFrame);
		gui::CompileRequest compile;
		compile.Display = screen;
		compile.Seconds = store.Time().Elapsed;
		frame.Compiled.Rebuild(*guiStore, compile);
		frame.SpatialCommands = frame.Compiled.Commands();
		std::erase_if(frame.SpatialCommands.Commands, [&](const gui::DrawCommand &command) {
			return guiStore->Get<gui::SpatialCanvas>(command.Collector) == nullptr;
		});
		frame.SpatialCollectors.clear();
		guiStore->Each<const gui::SpatialCanvas>([&](ecs::Entity collector,
													 const gui::SpatialCanvas &canvas) {
			frame.SpatialCollectors.push_back({collector, canvas});
		});
	}
}
