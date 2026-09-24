// Client-owned control composition and renderer/data-factory providers.

#include "ControlHooks.hpp"

#include "DataCaptureDriver.hpp"
#include "DataFactoryPausedPresentation.hpp"
#include "DisplayedSceneView.hpp"
#include "NamedCaptureView.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataCapture.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/PhysicsObservation.hpp>
#include <engine/control/features/RenderGraph.hpp>
#include <engine/control/features/Script.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/core/Assert.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Shooting.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/game/Project.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/SettingsMenu.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/input/Translate.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/render/DebugText.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/PipelineAdmission.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/TeleportRequest.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Postbox.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <client/Client.hpp>
#include <client/DataScriptPackage.hpp>
#include <client/DataScriptPackageTransaction.hpp>
#include <client/GuiActions.hpp>
#include <client/Replicated.hpp>
#include <client/WorldSystems.hpp>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <network/SessionKey.hpp>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace client {

	bool RenderGraphAdmissionFailure(
		const engine::render::Renderer &renderer,
		const engine::graph::PipelineDocument &document,
		std::string &failure
	) {
		const engine::render::PipelineAdmissionResult admission = renderer.ValidatePipelineDocument(document);
		if (admission) {
			failure.clear();
			return false;
		}
		failure = "unavailable: " + engine::render::FormatPipelineFailure(*admission.Failure);
		return true;
	}

	engine::control::features::VisibilitySnapshotReply
	VisibilityObservationSnapshot(const engine::render::Renderer &renderer) {
		const engine::render::VisibilitySnapshot snapshot = renderer.Visibility();
		engine::control::features::VisibilitySnapshotReply reply;
		reply.Frame = snapshot.Frame;
		reply.ViewSlot = snapshot.ViewSlot;
		reply.World = std::string(snapshot.World.Text());
		reply.Valid = snapshot.Valid;
		reply.Dropped = snapshot.Dropped;
		reply.DroppedExact = snapshot.DroppedExact;
		for (const engine::render::VisibilityObservation &row : snapshot.Observations) {
			reply.Observations.push_back(
				{std::string(row.World.Text()),
				 row.Entity,
				 engine::render::Describe(row.State),
				 engine::render::Describe(row.Cause)}
			);
		}
		return reply;
	}

	engine::control::HookLease
	ActivateVisibilityObservationHook(VisibilityObservationHookContext context, std::string &failure) {
		if (!context.Snapshot) {
			failure = "visibility snapshot provider is required";
			return {};
		}
		return context.Hooks.Activate(
			{
				.Id = "client.visibility-observation",
				.Revision = "v1",
				.Purpose = "Reads the most recent completed renderer visibility snapshot.",
				.Dependencies = {},
				.Limits = {},
			},
			[snapshot = std::move(context.Snapshot)](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::features::VisibilityObservationsTool(snapshot));
			},
			failure
		);
	}

	engine::control::HookLease
	ActivateTemporalSampleHook(FactoryReadHookContext context, std::string &failure) {
		return context.Hooks.Activate(
			{
				.Id = "client.temporal-sample",
				.Revision = "v1",
				.Purpose = "Reads retained camera and object poses from a data-factory snapshot.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe = context.Universe,
			 &session = context.Session](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::TemporalSampleTool(universe, session));
			},
			failure
		);
	}

	engine::control::HookLease ActivateRigExportHook(FactoryReadHookContext context, std::string &failure) {
		return context.Hooks.Activate(
			{
				.Id = "client.rig-export",
				.Revision = "v1",
				.Purpose = "Exports bounded data-rig records from a data-factory world.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe = context.Universe,
			 &session = context.Session](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::RigExportTool(universe, &session));
			},
			failure
		);
	}

	engine::control::HookLease
	ActivateDataAudioObservationHook(DataAudioObservationHookContext context, std::string &failure) {
		if (!context.Bridge) {
			failure = "audio observation bridge is required";
			return {};
		}
		return context.Hooks.Activate(
			{
				.Id = "client.audio-observation",
				.Revision = "v1",
				.Purpose = "Reads copied audio observations and bounded waveform chunks.",
				.Dependencies = {},
				.Limits = {},
			},
			[&universe = context.Universe,
			 bridge = std::move(context.Bridge),
			 &session = context.Session](engine::control::HookRegistration &registration) {
				auto rows = engine::control::DataAudioObservationTools(universe, bridge, &session);
				registration.Add(std::move(rows.Metadata));
				registration.Add(std::move(rows.Observation));
				registration.Add(std::move(rows.Waveform));
			},
			failure
		);
	}

	engine::control::HookLease ActivateDataCaptureHook(DataCaptureHookContext context, std::string &failure) {
		return context.Surface.ActivateHook(
			{.Id = "client.data-capture",
			 .Revision = "v1",
			 .Purpose = "Owns client capture tickets.",
			 .Dependencies = {"client.data-factory-lifecycle"},
			 .Limits = {}},
			[context](engine::control::HookRegistration &registration) {
				for (const char *name :
					 {"poll_capture",
					  "get_resource",
					  "release_capture",
					  "cancel_capture",
					  "poll_view_camera_mutation",
					  "cancel_view_camera_mutation"})
					registration.KeepToolDuringDrain(name);
				registration.SetDrain([bridge = context.Bridge] {
					if (bridge == nullptr) return true;
					bridge->CancelPending();
					bridge->Pump();
					return !bridge->HasOutstanding();
				});
				registration.SetRelease([context] {
					context.Surface.SetDataCaptureAvailabilityProvider({});
				});
				context.Surface.AddDataCaptureTools(context.Session, context.Bridge);
				context.Surface.SetDataCaptureAvailabilityProvider([context] {
					const auto hooks = context.Surface.Hooks().Active();
					const bool active = std::any_of(hooks.begin(), hooks.end(), [](const auto &hook) {
						return hook.Descriptor.Id == "client.data-capture" &&
							   hook.State == engine::control::HookState::Active;
					});
					if (!active || !context.Bridge) return engine::control::DataCaptureAvailability{};
					const auto capabilities = context.Bridge->Capabilities();
					return engine::control::DataCaptureAvailability{
						.Available = capabilities.Available,
						.Channels = capabilities.Channels,
						.Detail = capabilities.Detail,
					};
				});
			},
			failure
		);
	}

	void Client::ConfigureControlHooks() {
		struct PermanentHook {
			const char *Id;
			std::function<void()> Install;
		};
		const std::array manifest{
			PermanentHook{
				"builtin.universe", [this] { ControlSurface.AddUniverseTools(*Universe_, true, true); }
			},
			PermanentHook{"builtin.architecture", [this] { ControlSurface.AddArchitectureTools(); }},
			PermanentHook{"builtin.script", [this] { ControlSurface.AddScriptTools(); }},
			PermanentHook{"builtin.diagnostics", [this] { ControlSurface.AddDiagnosticTools(); }},
			PermanentHook{"builtin.resources", [this] { ControlSurface.AddStandardResources(); }},
			PermanentHook{"builtin.prompts", [this] { ControlSurface.AddStandardPrompts(); }},
			PermanentHook{"builtin.discovery", [this] { ControlSurface.AddDiscoveryTools(); }},
		};
		PermanentControlHooks.reserve(manifest.size());
		for (const PermanentHook &hook : manifest) {
			std::string failure;
			auto lease = ControlSurface.ActivateHook(
				{.Id = hook.Id,
				 .Revision = "v1",
				 .Purpose = "Built-in feature registration.",
				 .Dependencies = {},
				 .Limits = {}},
				[&hook](engine::control::HookRegistration &) { hook.Install(); },
				failure
			);
			if (!lease.IsValid()) throw std::runtime_error(failure);
			PermanentControlHooks.push_back(std::move(lease));
		}
		struct InputControlHookContext {
			Client &Host;
			engine::control::Surface &Surface;
		};
		const InputControlHookContext inputContext{.Host = *this, .Surface = ControlSurface};
		std::string inputFailure;
		InputControlHook.emplace(inputContext.Surface.ActivateHook(
			{.Id = "client.input",
			 .Revision = "v1",
			 .Purpose = "Queues client input through the normal event boundary.",
			 .Dependencies = {},
			 .Limits = {{"queued_events", 64}}},
			[inputContext](engine::control::HookRegistration &) {
				inputContext.Surface.AddInputTools(
					[inputContext](
						const engine::control::InputAutomationEvent &event, std::string &failure
					) -> nlohmann::json {
						Client &host = inputContext.Host;
						if (host.PendingControlInput.size() >= 64) {
							failure = "input queue is full";
							return nullptr;
						}
						if (event.Kind == engine::control::InputAutomationKind::Key &&
							SDL_GetKeyFromName(event.Key.c_str()) == SDLK_UNKNOWN &&
							SDL_GetScancodeFromName(event.Key.c_str()) == SDL_SCANCODE_UNKNOWN) {
							failure = "key is not a recognized SDL key name";
							return nullptr;
						}
						if (event.Kind == engine::control::InputAutomationKind::MouseMove ||
							event.Kind == engine::control::InputAutomationKind::MouseButton) {
							int width = host.Settings.Width;
							int height = host.Settings.Height;
							if (host.Window != nullptr) (void)SDL_GetWindowSize(host.Window, &width, &height);
							if (event.X < 0.0f || event.Y < 0.0f || event.X >= width || event.Y >= height) {
								failure = "pointer coordinates are outside the client area";
								return nullptr;
							}
						}
						host.PendingControlInput.push_back(event);
						return nlohmann::json{{"queued", true}};
					}
				);
			},
			inputFailure
		));
		if (!InputControlHook->IsValid()) {
			ENGINE_ERROR("control: input hook did not activate: {}", inputFailure);
			InputControlHook.reset();
		}
		struct RenderGraphControlHookContext {
			engine::control::Surface &Surface;
		};
		const RenderGraphControlHookContext renderGraphContext{.Surface = ControlSurface};
		std::string renderGraphFailure;
		RenderGraphHook.emplace(renderGraphContext.Surface.ActivateHook(
			{.Id = "client.render-graph",
			 .Revision = "v1",
			 .Purpose = "Reports the active client render graph.",
			 .Dependencies = {},
			 .Limits = {}},
			[renderGraphContext](engine::control::HookRegistration &) {
				engine::control::features::RenderGraph(renderGraphContext.Surface);
			},
			renderGraphFailure
		));
		if (!RenderGraphHook->IsValid()) {
			ENGINE_ERROR("control: render graph hook did not activate: {}", renderGraphFailure);
			RenderGraphHook.reset();
		}
		if (DataFactory) {
			struct DataFactoryControlHookContext {
				Client &Host;
				engine::control::Surface &Surface;
				engine::world::Universe &Universe;
				engine::world::DataFactorySession &Session;
				std::shared_ptr<engine::render::ScriptDataCaptureBridge> Capture;
			};
			const DataFactoryControlHookContext dataFactoryContext{
				.Host = *this,
				.Surface = ControlSurface,
				.Universe = *Universe_,
				.Session = *DataFactory,
				.Capture = DataCapture,
			};
			std::string lifecycleFailure;
			DataFactoryLifecycleHook.emplace(dataFactoryContext.Surface.ActivateHook(
				{.Id = "client.data-factory-lifecycle",
				 .Revision = "v1",
				 .Purpose = "Owns the client data-factory lifecycle tools.",
				 .Dependencies = {},
				 .Limits = {}},
				[dataFactoryContext](engine::control::HookRegistration &) {
					dataFactoryContext.Surface.AddDataFactoryTools(dataFactoryContext.Session);
				},
				lifecycleFailure
			));
			if (!DataFactoryLifecycleHook->IsValid()) {
				ENGINE_ERROR("control: data-factory lifecycle hook did not activate: {}", lifecycleFailure);
				DataFactoryLifecycleHook.reset();
			}
			std::string physicsObservationFailure;
			PhysicsObservationHook.emplace(dataFactoryContext.Surface.ActivateHook(
				{.Id = "client.data-factory.physics-observation",
				 .Revision = "v1",
				 .Purpose = "Reads completed fixed-step physics observations.",
				 .Dependencies = {"client.data-factory-lifecycle"},
				 .Limits = {{"records", engine::physics::PhysicsObservationLog::CAPACITY}}},
				[dataFactoryContext](engine::control::HookRegistration &) {
					engine::control::AddPhysicsObservationTools(
						dataFactoryContext.Surface, dataFactoryContext.Session
					);
				},
				physicsObservationFailure
			));
			if (!PhysicsObservationHook->IsValid()) {
				ENGINE_ERROR(
					"control: physics observation hook did not activate: {}", physicsObservationFailure
				);
				PhysicsObservationHook.reset();
			}
			std::string audioObservationFailure;
			DataAudioObservationHook.emplace(ActivateDataAudioObservationHook(
				{.Hooks = dataFactoryContext.Surface.Hooks(),
				 .Universe = dataFactoryContext.Universe,
				 .Bridge = DataAudio,
				 .Session = dataFactoryContext.Session},
				audioObservationFailure
			));
			if (!DataAudioObservationHook->IsValid()) {
				ENGINE_ERROR("control: audio observation hook did not activate: {}", audioObservationFailure);
				DataAudioObservationHook.reset();
			}
			std::string scriptPackageFailure;
			DataScriptPackageHook.emplace(dataFactoryContext.Surface.ActivateHook(
				{.Id = "client.data-script-package",
				 .Revision = "v1",
				 .Purpose = "Runs bounded data-factory script packages.",
				 .Dependencies = {"client.data-factory-lifecycle"},
				 .Limits = {}},
				[dataFactoryContext](engine::control::HookRegistration &) {
					AddDataScriptPackageTool(
						dataFactoryContext.Surface,
						[dataFactoryContext](const engine::script::DataScriptRequest &request) {
							Client &host = dataFactoryContext.Host;
							return ExecuteDataScriptPackageTransaction(
								{
									.Universe = dataFactoryContext.Universe,
									.Session = dataFactoryContext.Session,
									.RuntimeOf =
										[&host](engine::world::WorldId world) -> engine::script::Runtime * {
										const auto runtime =
											std::ranges::find_if(host.Runtimes, [world](const auto &entry) {
												return entry.first == world;
											});
										return runtime == host.Runtimes.end() ? nullptr
																			  : runtime->second.get();
									},
									.DiscardRuntime =
										[&host](engine::world::WorldId world) {
											std::erase_if(host.Runtimes, [world](const auto &entry) {
												return entry.first == world;
											});
										},
									.MakeRuntime =
										[](engine::ecs::Store &store,
										   const engine::script::RuntimeLimits &limits) {
											return engine::script::MakeRuntime(
												store, engine::script::Language::Luau, limits
											);
										},
									.RunPackage = engine::script::RunDataScriptPackage,
									.InstallSystems =
										[&host](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
											InstallPresentation(store, systems, host.Settings.Entities);
											(void)EnsureLocalPlayer(store);
											(void)RestoreDefaultCameraMovement(store, systems);
											(void)InstallDefaultCamera(store, systems);
											InstallClientWorldSystems(store, systems);
										},
									.Admit =
										[](std::string_view source,
										   std::string_view entry,
										   std::string &error) {
											return engine::script::CheckDataScriptPackageSource(
												engine::script::Language::Luau, source, entry, error
											);
										},
									.Role = engine::script::HostRole::OfBoth(),
									.Present = true,
								},
								request
							);
						}
					);
				},
				scriptPackageFailure
			));
			if (!DataScriptPackageHook->IsValid()) {
				ENGINE_ERROR("control: data script package hook did not activate: {}", scriptPackageFailure);
				DataScriptPackageHook.reset();
			}
			std::string captureFailure;
			DataCaptureHook.emplace(ActivateDataCaptureHook(
				{.Surface = dataFactoryContext.Surface,
				 .Session = dataFactoryContext.Session,
				 .Bridge = dataFactoryContext.Capture},
				captureFailure
			));
			if (!DataCaptureHook->IsValid()) DataCaptureHook.reset();
			std::string dataSceneFailure;
			DataSceneHook.emplace(dataFactoryContext.Surface.ActivateHook(
				{.Id = "client.data-scene",
				 .Revision = "v1",
				 .Purpose = "Reads and exports the factory scene.",
				 .Dependencies = {"client.data-factory-lifecycle"},
				 .Limits = {}},
				[dataFactoryContext](engine::control::HookRegistration &) {
					dataFactoryContext.Surface.AddDataSceneTools(
						dataFactoryContext.Universe,
						dataFactoryContext.Capture,
						&dataFactoryContext.Session,
						[dataFactoryContext](
							std::string_view world, std::string_view name, engine::assets::MeshData &out
						) {
							const auto status = dataFactoryContext.Host.Renderer.CopyMesh(
								engine::core::Name(name),
								out,
								engine::script::MAX_GLTF_EXPORT_VERTICES,
								engine::script::MAX_GLTF_EXPORT_INDICES,
								engine::core::Name(world)
							);
							switch (status) {
							case engine::render::MeshCopyStatus::Copied:
								return engine::script::GltfMeshSourceStatus::Available;
							case engine::render::MeshCopyStatus::OverLimit:
								return engine::script::GltfMeshSourceStatus::OverLimit;
							case engine::render::MeshCopyStatus::Packed:
								return engine::script::GltfMeshSourceStatus::Unsupported;
							case engine::render::MeshCopyStatus::Invalid:
								return engine::script::GltfMeshSourceStatus::Invalid;
							case engine::render::MeshCopyStatus::Missing:
								return engine::script::GltfMeshSourceStatus::Missing;
							}
							return engine::script::GltfMeshSourceStatus::Unsupported;
						},
						[dataFactoryContext](
							std::string_view world, std::string_view name, engine::assets::TextureData &out
						) {
							const auto status = dataFactoryContext.Host.Renderer.CopyTexture(
								engine::core::Name(name),
								out,
								engine::script::MAX_GLTF_EXPORT_SOURCE_TEXTURE_BYTES,
								engine::core::Name(world)
							);
							switch (status) {
							case engine::render::TextureCopyStatus::Copied:
								return engine::script::GltfTextureSourceStatus::Available;
							case engine::render::TextureCopyStatus::OverLimit:
								return engine::script::GltfTextureSourceStatus::OverLimit;
							case engine::render::TextureCopyStatus::Unsupported:
								return engine::script::GltfTextureSourceStatus::Unsupported;
							case engine::render::TextureCopyStatus::Invalid:
								return engine::script::GltfTextureSourceStatus::Invalid;
							case engine::render::TextureCopyStatus::Missing:
								return engine::script::GltfTextureSourceStatus::Missing;
							}
							return engine::script::GltfTextureSourceStatus::Unsupported;
						}
					);
				},
				dataSceneFailure
			));
			if (!DataSceneHook->IsValid()) {
				ENGINE_ERROR("control: data scene hook did not activate: {}", dataSceneFailure);
				DataSceneHook.reset();
			}
			std::string sceneRenderingFailure;
			SceneRenderingHook.emplace(dataFactoryContext.Surface.ActivateHook(
				{.Id = "client.scene-rendering",
				 .Revision = "v1",
				 .Purpose = "Reads camera calibration for the rendered factory scene.",
				 .Dependencies = {"client.data-factory-lifecycle"},
				 .Limits = {}},
				[dataFactoryContext](engine::control::HookRegistration &registration) {
					registration.Add(
						engine::control::features::CameraRenderingDataTool(
							dataFactoryContext.Universe, &dataFactoryContext.Session
						)
					);
				},
				sceneRenderingFailure
			));
			if (!SceneRenderingHook->IsValid()) {
				ENGINE_ERROR("control: scene-rendering hook did not activate: {}", sceneRenderingFailure);
				SceneRenderingHook.reset();
			}
			std::string temporalSampleFailure;
			TemporalSampleHook.emplace(ActivateTemporalSampleHook(
				{.Hooks = ControlSurface.Hooks(), .Universe = *Universe_, .Session = *DataFactory},
				temporalSampleFailure
			));
			if (!TemporalSampleHook->IsValid()) {
				ENGINE_ERROR("control: temporal sample hook did not activate: {}", temporalSampleFailure);
				TemporalSampleHook.reset();
			}
			std::string rigExportFailure;
			RigExportHook.emplace(ActivateRigExportHook(
				{.Hooks = ControlSurface.Hooks(), .Universe = *Universe_, .Session = *DataFactory},
				rigExportFailure
			));
			if (!RigExportHook->IsValid()) {
				ENGINE_ERROR("control: rig export hook did not activate: {}", rigExportFailure);
				RigExportHook.reset();
			}
		}
		std::string visibilityFailure;
		VisibilityObservationHook.emplace(ActivateVisibilityObservationHook(
			{.Hooks = ControlSurface.Hooks(),
			 .Snapshot = [this] { return VisibilityObservationSnapshot(Renderer); }},
			visibilityFailure
		));
		if (!VisibilityObservationHook->IsValid()) {
			ENGINE_ERROR("control: visibility observation hook did not activate: {}", visibilityFailure);
			VisibilityObservationHook.reset();
		}
	}

}
