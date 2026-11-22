// The built-in node table and dispatch of a view recording, and `Finish`,
// which runs the compiled graph and submits the view.

#include "DisplayColour.hpp"
#include "SurfaceScale.hpp"
#include "capture/ViewRecording.hpp"
#include "presentation/GpuCommandIdentity.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace engine::render {
	const ContextNodeTable<ViewRecording> &ViewRecording::BuiltInNodes() {
		static const ContextNodeTable<ViewRecording> nodes = [] {
			ContextNodeTable<ViewRecording> table;
			table.SelectFamily(RetainedUpload);
			RegisterUploadNodes(table);
			table.SelectFamily(RetainedShadow);
			RegisterShadowNodes(table);
			table.SelectFamily(RetainedMirror);
			RegisterMirrorNodes(table);
			table.SelectFamily(RetainedPortal);
			RegisterPortalNodes(table);
			table.SelectFamily(RetainedSurface);
			RegisterSurfaceNodes(table);
			table.SelectFamily(RetainedAuthored);
			RegisterAuthoredNodes(table);
			table.SelectFamily(RetainedGeometry);
			RegisterGeometryNodes(table);
			table.SelectFamily(RetainedShading);
			RegisterShadingNodes(table);
			table.SelectFamily(0);
			RegisterOutputNodes(table);
			return table;
		}();
		return nodes;
	}

	NodeDispatch ViewRecording::Dispatch() {
		CustomHandlers.clear();
		CustomHandlers.reserve(Owner.CustomNodeHandlers.size());
		for (const auto &installed : Owner.CustomNodeHandlers)
			CustomHandlers.emplace_back(installed.Kind, installed.Handler);
		return {
			this,
			[](void *recording, core::Name kind) {
				return static_cast<ViewRecording *>(recording)->HasNode(kind);
			},
			[](void *recording, const graph::RunContext &context) {
				return static_cast<ViewRecording *>(recording)->RunNode(context);
			},
		};
	}

	bool ViewRecording::HasNode(core::Name kind) const {
		if (BuiltInNodes().Has(kind)) return true;
		for (const auto &[registered, handler] : CustomHandlers)
			if (registered == kind) return true;
		return false;
	}

	bool ViewRecording::RunNode(const graph::RunContext &context) {
		const auto &nodes = BuiltInNodes();
		const uint16_t families = Request.Damage.Scene ? UINT16_MAX : Pipeline->RetainedFamilies;
		const auto resolved = nodes.Resolve(context.Kind, families);
		if (resolved.Run != nullptr) return (this->*resolved.Run)(context);
		for (const auto &[kind, handler] : CustomHandlers)
			if (kind == context.Kind) return handler(context);
		return resolved.Known;
	}

	void ViewRecording::Finish() {
		Impl *const State = this->State;
		FrameResult &result = Result;
		const Impl::InstalledPipeline *const selectedPipeline = Pipeline;
		SDL_GPUCommandBuffer *&command = Command;
		SDL_GPUTexture *const swapchain = Swapchain;
		FrameOverlayHook *const hostOverlayHook = Request.HostOverlayHook;
		const size_t targetSlot = Request.TargetSlot;
		const bool present = Request.Present;
		const uint64_t world = Request.World;
		const bool offscreen = Offscreen;
		const uint32_t width = Width;
		const uint32_t height = Height;
		const uint32_t sceneWidth = SceneWidth;
		const uint32_t sceneHeight = SceneHeight;
		const bool drawHostOverlay = DrawHostOverlay;
		uint32_t &timingSlot = TimingSlot;
		SDL_GPUColorTargetInfo &windowTarget = WindowTarget;
		const Impl::NamedTexture &authoredCapture = AuthoredCapture;
		const std::filesystem::path &authoredCapturePath = AuthoredCapturePath;

		const auto closePass = [this] { ClosePass(); };
		const auto endIncompleteView = [this] { EndIncompleteView(); };
		const auto fixedTexture = [this](core::Name resource, size_t slot) {
			return FixedTexture(resource, slot);
		};
		auto &sceneSlot = State->SlotAt(targetSlot);
		const bool retainedBindings = !Request.Damage.Scene && !sceneSlot.ForwardUnavailable &&
									  sceneSlot.ForwardPipeline == Pipeline->Name &&
									  sceneSlot.ForwardRevision == Pipeline->Revision &&
									  sceneSlot.ForwardWorld == Request.World &&
									  sceneSlot.ForwardedTextures.size() == Pipeline->Graph.ResourceCount();
		auto &working = sceneSlot.ForwardWorking;
		working.Textures.resize(Pipeline->Graph.ResourceCount());
		if (retainedBindings)
			std::copy(
				sceneSlot.ForwardedTextures.begin(),
				sceneSlot.ForwardedTextures.end(),
				working.Textures.begin()
			);
		else
			std::fill(working.Textures.begin(), working.Textures.end(), Impl::NamedTexture{});
		ForwardedTextures = working.Textures;
		working.Pipeline = Pipeline->Name;
		working.Revision = Pipeline->Revision;
		working.World = Request.World;
		working.Ready = false;
		working.Command = GpuCommandIdentity(command);
		Activity = retainedBindings ? sceneSlot.Activity : PassActivity{};
		if (!retainedBindings) {
			Activity.Valid = true;
			Activity.Sky = GraphEnabled(core::Name("sky")) && State->Caps.HasCompute &&
						   (CurrentLighting.EnvironmentState.Skybox != scene::SkyboxSource::None ||
							CurrentLighting.EnvironmentState.HasAtmosphere);
			Activity.Fog = GraphEnabled(core::Name("fog")) &&
						   (Uniforms.VolumeCount.x >= 0.5f || Uniforms.CloudCentreEnabled.w >= 0.5f);
			Activity.ShaderLenses = GraphEnabled(core::Name("shader-lenses")) && LensGroupCount != 0;
			Activity.DepthOfField = GraphEnabled(core::Name("dof")) && State->DepthOfFieldIntensity > 0.0f &&
									State->DepthOfFieldRadius > 0.0f;
			Activity.Bloom = GraphEnabled(core::Name("bloom")) && State->BloomIntensity > 0.0f &&
							 State->BloomRadius > 0.0f;
			for (size_t index = 0; index < scene::MAX_SURFACES; ++index) {
				const auto &run = CameraRuns[index];
				if (!HaveInstances || (run.OpaqueCount == 0 && run.BlendedCount == 0)) continue;
				if (PortalOf[index] != nullptr && HavePortals && GraphEnabled(core::Name("portal-overlay")))
					Activity.PortalOverlay = true;
				if (PortalOf[index] == nullptr && GraphEnabled(core::Name("mirror-overlay")))
					Activity.MirrorOverlay = true;
			}
			Activity.Grid = Request.Source->Grid.Enabled && Request.Source->Grid.Strength != 0.0f;
			Activity.Particles = ParticleCount != 0;
			Activity.Ribbons = RibbonCount != 0;
			const bool worldInterface = DrawInterface && Request.GameInterfaceHook != nullptr &&
										Request.GameInterfaceHook->AffectsScene();
			Activity.Transparent =
				GraphEnabled(core::Name("transparent")) &&
				(Activity.Grid || PlainTransparent != 0 || worldInterface || Activity.Particles ||
				 Activity.Ribbons ||
				 (Request.Source->SpatialSnapshotSequence != 0 && Request.Source->SnapshotLayersKnown &&
				  Request.Source->CaptureSpatialDepth));
		}
		Activity.GodRays = false;
		if (GraphEnabled(core::Name("god-rays")) && State->GodRayIntensity > 0.0f &&
			State->GodRayRadius > 0.0f) {
			const auto eye = Request.VisibilityCameraFrame.Position;
			// Project at the view's far distance so an ordinary short camera still sees its sun.
			const float distance = std::max(DrawCamera.NearPlane * 2.0f, DrawCamera.FarPlane * 0.99f);
			GodRayClip = Matrices.ViewProjection * glm::vec4{
													   eye.X - State->Sun.x * distance,
													   eye.Y - State->Sun.y * distance,
													   eye.Z - State->Sun.z * distance,
													   1.0f
												   };
			Activity.GodRays = GodRayClip.w > 0.0f && std::abs(GodRayClip.x) <= GodRayClip.w &&
							   std::abs(GodRayClip.y) <= GodRayClip.w && GodRayClip.z >= 0.0f &&
							   GodRayClip.z <= GodRayClip.w;
		}

		// **The whole of the frame's recording, and it was the widest blank of
		// the lot.** Everything below this line is `ExecuteRenderStage`
		// walking the compiled graph and dispatching into this recording, and
		// neither `GraphRunner::Run` nor most of the handlers opened a span - so
		// the gbuffer, the lighting, the tonemap and the present all landed
		// inside `Renderer::RenderView` with nothing to attribute them to.
		// `GraphRunner::Run` now names each node; this is the bar they sit under.
		ENGINE_PROFILE_CAT("execute graph", core::ProfileCategory::Render);

		NodeProfileHooks profile;
		profile.Enabled = [this](const graph::RunContext &context) { return ProfileEnabled(context); };
		profile.Begin = [this](const graph::RunContext &context) { BeginNodeProfile(context); };
		profile.End = [this](const graph::RunContext &context) { return EndNodeProfile(context); };

		GraphRunner frameRunner(Dispatch(), State->ProfileTier, std::move(profile));

		struct RetainedRunner final : graph::NodeRunner {
			GraphRunner &Inner;
			std::function<bool(const graph::RunContext &)> Enabled;
			RetainedRunner(GraphRunner &inner, decltype(Enabled) enabled)
				: Inner(inner), Enabled(std::move(enabled)) {}
			bool Run(const graph::RunContext &context) override {
				return !Enabled || Enabled(context) ? Inner.Run(context) : true;
			}
		};
		RetainedRunner retained(frameRunner, [this](const graph::RunContext &context) {
			const bool enabled =
				Request.Damage.Scene ||
				(context.Node.Value != 0 && context.Node.Value <= Pipeline->RetainedNodes.size() &&
				 Pipeline->RetainedNodes[context.Node.Value - 1] != 0);
			if (enabled && (EmptyGBufferNormals || EmptyHardwareDepth != nullptr ||
							FarLinearDepth != nullptr || OpaqueSkyTexture != nullptr ||
							ClearedInterfaceTexture != nullptr || !this->State->Environments.empty())) {
				// Authored handlers can write attachments without calling GraphTexture.
				for (const auto resource : context.Writes) {
					const auto target = ResourceTexture(resource, GraphTextureSlot(context), false);
					const auto *node = Pipeline->Graph.Find(context.Node);
					InvalidateBackgroundTexture(target.Texture, node != nullptr ? node->Kind : core::Name{});
				}
			}
			return enabled;
		});

		State->StageProbe.ActivateView(
			State->FrameCounter, Request.Source->WorldName.Text(), Request.TargetSlot, State->BatchFinal
		);
		struct ProbedRunner final : graph::NodeRunner {
			graph::NodeRunner &Inner;
			std::function<void(const graph::RunContext &, bool, bool)> Probe;
			ProbedRunner(graph::NodeRunner &inner, decltype(Probe) probe)
				: Inner(inner), Probe(std::move(probe)) {}
			bool Run(const graph::RunContext &context) override {
				Probe(context, true, true);
				const bool accepted = Inner.Run(context);
				Probe(context, false, accepted);
				return accepted;
			}
		};
		struct SnapshotTextureTag {
			SDL_GPUTexture *Texture = nullptr;
			uint8_t Bits = 0;
		};
		std::array<SnapshotTextureTag, graph::RenderGraph::MAXIMUM_RESOURCES> snapshotTextures{};
		size_t snapshotTextureCount = 0;
		std::array<uint8_t, graph::RenderGraph::MAXIMUM_RESOURCE_USES> snapshotInputs{};
		std::array<Impl::NamedTexture, graph::RenderGraph::MAXIMUM_RESOURCE_USES> snapshotInputTextures{};
		bool snapshotOverflow = false, snapshotDepthBefore = false, snapshotProven = false;
		bool snapshotSawTransparent = false, snapshotSawInterface = false, snapshotSawOutput = false;
		bool snapshotScreenFailed = false, snapshotOutputFailed = false;
		const auto lookupSnapshot = [&](SDL_GPUTexture *texture) -> uint8_t {
			if (!texture) return 0;
			for (size_t i = 0; i < snapshotTextureCount; ++i)
				if (snapshotTextures[i].Texture == texture) return snapshotTextures[i].Bits;
			return 0;
		};
		const auto markSnapshot = [&](SDL_GPUTexture *texture, uint8_t bits) {
			if (!texture) return;
			for (size_t i = 0; i < snapshotTextureCount; ++i) {
				if (snapshotTextures[i].Texture == texture) {
					snapshotTextures[i].Bits = bits;
					return;
				}
			}
			if (snapshotTextureCount == snapshotTextures.size()) {
				snapshotOverflow = true;
				return;
			}
			snapshotTextures[snapshotTextureCount++] = {texture, bits};
		};
		ProbedRunner probed(
			retained,
			[this,
			 State,
			 &snapshotInputs,
			 &snapshotInputTextures,
			 &snapshotOverflow,
			 &snapshotDepthBefore,
			 &snapshotProven,
			 &snapshotSawTransparent,
			 &snapshotSawInterface,
			 &snapshotSawOutput,
			 &snapshotScreenFailed,
			 &snapshotOutputFailed,
			 &lookupSnapshot,
			 &markSnapshot](const graph::RunContext &context, bool before, bool accepted) {
				if (Request.Source->SpatialSnapshotSequence != 0 && Request.Damage.SceneImage()) {
					if (before) {
						SnapshotNodePerformed = false;
						SnapshotScreenStatus = OverlayRecordStatus::Unknown;
						SnapshotNodeComposited = false;
						SnapshotNodeOutput = nullptr;
						SnapshotOutputValid = false;
						if (context.Kind == core::Name("output-image")) snapshotProven = false;
						snapshotDepthBefore = SpatialDepthRecorded;
						if (context.Reads.size() > snapshotInputs.size())
							snapshotOverflow = true;
						else
							for (size_t i = 0; i < context.Reads.size(); ++i) {
								snapshotInputTextures[i] = GraphTexture(context.Reads[i], context, false);
								snapshotInputs[i] = lookupSnapshot(snapshotInputTextures[i].Texture);
							}
					} else if (accepted && !snapshotOverflow) {
						const bool executed =
							Request.Damage.Scene || (context.Node.Value != 0 &&
													 context.Node.Value <= Pipeline->RetainedNodes.size() &&
													 Pipeline->RetainedNodes[context.Node.Value - 1] != 0);
						if (executed) {
							const auto kind = context.Kind;
							if (kind == core::Name("transparent")) snapshotSawTransparent = true;
							if (kind == core::Name("interface")) {
								snapshotSawInterface = true;
							}
							if (kind == core::Name("output-image")) {
								snapshotSawOutput = true;
								snapshotOutputFailed = !SnapshotOutputValid;
							}
							const auto *node = Pipeline->Graph.Find(context.Node);
							size_t primary = 0;
							if (kind == core::Name("present") || kind == core::Name("output-image")) {
								while (primary < context.Reads.size() &&
									   !snapshotInputTextures[primary].IsValid())
									++primary;
							} else if (kind == core::Name("shader-lenses")) {
								primary = node ? size_t(
													 std::find(
														 node->ReadPorts.begin(),
														 node->ReadPorts.end(),
														 core::Name("colour")
													 ) -
													 node->ReadPorts.begin()
												 )
											   : context.Reads.size();
							}
							const auto first =
								primary < context.Reads.size() ? snapshotInputs[primary] : uint8_t{0};
							uint8_t output = 0;
							if (kind == core::Name("transparent") && !snapshotDepthBefore &&
								SpatialDepthRecorded &&
								(Request.Source->SnapshotSpatialCommands
									 ? SpatialWorldStatus == OverlayRecordStatus::Complete
									 : SnapshotLayerStatus == OverlayRecordStatus::Complete)) {
								output = 4;
								if (SpatialWorldStatus == OverlayRecordStatus::Complete) output |= 1;
							} else if (kind == core::Name("interface") && SnapshotNodePerformed)
								output = 2;
							else if (kind == core::Name("interface") &&
									 SnapshotScreenStatus == OverlayRecordStatus::Failed)
								output = 8;
							else if (kind == core::Name("overlay") && SnapshotNodeComposited &&
									 context.Reads.size() >= 2)
								output = (first & (1 | 4)) | (snapshotInputs[1] & (2 | 8));
							else if (kind == core::Name("present") && SnapshotNodePerformed)
								output = first;
							else if (kind == core::Name("shader-lenses") && !Activity.ShaderLenses)
								output = first;
							else if (kind == core::Name("dof") && !Activity.DepthOfField)
								output = first;
							else if (kind == core::Name("god-rays") && !Activity.GodRays)
								output = first;
							else if (kind == core::Name("tonemap") && State->BloomIntensity == 0.0f &&
									 State->PostProcessPipelines.find(
										 State->ShaderOwnerOf(*Request.Source, {}).Id()
									 ) == State->PostProcessPipelines.end())
								output = first;
							if (kind == core::Name("output-image") && SnapshotOutputValid) {
								const uint8_t wanted = (Request.Source->CaptureSpatialDepth ? 4 : 0) |
													   (Request.Source->SnapshotSpatialCommands ? 1 : 0) |
													   (Request.Source->SnapshotScreenCommands ? 2 : 0);
								snapshotProven = wanted != 0 && (first & wanted) == wanted;
								snapshotScreenFailed = (first & 8) != 0;
							}
							SDL_GPUTexture *written = SnapshotNodeOutput;
							if (!written && (kind == core::Name("shader-lenses") ||
											 kind == core::Name("dof") || kind == core::Name("god-rays"))) {
								for (size_t w = 0; w < context.Writes.size(); ++w) {
									if (kind == core::Name("shader-lenses") &&
										(!node || w >= node->WritePorts.size() ||
										 node->WritePorts[w] != core::Name("colour")))
										continue;
									const auto candidate = GraphTexture(context.Writes[w], context, false);
									if (candidate.IsValid()) {
										written = candidate.Texture;
										break;
									}
								}
							}
							for (size_t w = 0; w < context.Writes.size(); ++w) {
								const auto target = GraphTexture(context.Writes[w], context, false);
								uint8_t bits = target.Texture == written ? output : 0;
								if (kind == core::Name("shader-lenses") &&
									(!node || w >= node->WritePorts.size() ||
									 node->WritePorts[w] != core::Name("colour")))
									bits = 0;
								if (bits && !context.Reads.empty() && kind != core::Name("transparent") &&
									kind != core::Name("interface") &&
									(primary >= context.Reads.size() ||
									 !snapshotInputTextures[primary].IsValid() || !target.IsValid() ||
									 snapshotInputTextures[primary].Width != target.Width ||
									 snapshotInputTextures[primary].Height != target.Height))
									bits = 0;
								if (kind == core::Name("overlay") &&
									(context.Reads.size() < 2 || !snapshotInputTextures[1].IsValid() ||
									 snapshotInputTextures[1].Width != target.Width ||
									 snapshotInputTextures[1].Height != target.Height))
									bits = 0;
								markSnapshot(target.Texture, bits);
							}
						}
					}
				}
				if (!State->StageProbe.Enabled(State->FrameCounter, Request.TargetSlot)) return;
				ClosePass();
				const auto save = [&](const Impl::NamedTexture &texture, std::string_view resource) {
					if (!State->StageProbe.Wants(context.Name.Text(), resource)) return;
					std::ostringstream metadata;
					metadata << "\"pipeline\":" << RenderStageProbe::Quote(Pipeline->Name.Text())
							 << ",\"stage\":" << RenderStageProbe::Quote(context.Name.Text())
							 << ",\"resource\":" << RenderStageProbe::Quote(resource)
							 << ",\"world\":" << RenderStageProbe::Quote(Request.Source->WorldName.Text())
							 << ",\"view_slot\":" << Request.TargetSlot
							 << ",\"eye_handle\":" << Request.Source->EyeImage
							 << ",\"stage_ran\":" << (Result.Ran(context.Name) ? "true" : "false")
							 << ",\"camera_position\":[" << Request.Source->CameraFrame.Position.X << ","
							 << Request.Source->CameraFrame.Position.Y << ","
							 << Request.Source->CameraFrame.Position.Z << "]"
							 << ",\"before\":" << (before ? "true" : "false")
							 << ",\"node_accepted\":" << (accepted ? "true" : "false");
					if (State->StageProbe.TransitionMode())
						metadata << ",\"transition_from\":"
								 << RenderStageProbe::Quote(State->StageProbe.TransitionFrom)
								 << ",\"transition_to\":"
								 << RenderStageProbe::Quote(State->StageProbe.TransitionTo);
					State->StageProbe.Record(
						State->Device,
						Command,
						State->FrameCounter,
						metadata.str(),
						texture.Texture,
						texture.Width,
						texture.Height,
						texture.Format,
						std::string(context.Name.Text()) + " / " + std::string(resource)
					);
				};
				if (before) {
					if (context.Kind != core::Name("eye-image")) return;
					for (const auto &image : State->ImportedPortals)
						if (image.Handle != 0 && image.Handle == Request.Source->EyeImage)
							save(
								{image.Texture,
								 image.Width,
								 image.Height,
								 SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
								"imported-eye-input"
							);
					return;
				}
				if (context.Writes.empty()) save({}, "no-image-output");
				for (const auto resource : context.Writes) {
					const auto *description = Pipeline->Graph.FindResourceDesc(resource);
					save(
						GraphTexture(resource, context, false),
						description ? description->Name.Text() : "unknown"
					);
				}
			}
		);
		graph::NodeRunner &runner = (State->StageProbe.Enabled(State->FrameCounter, Request.TargetSlot) ||
									 Request.Source->SpatialSnapshotSequence != 0)
										? static_cast<graph::NodeRunner &>(probed)
										: retained;
		bool dispatched = false;
		if (State->BatchActive) {
			const bool frameSetup =
				Request.Damage.Scene && State->PreparedScopes.NeedsFrame(selectedPipeline);
			const bool isolatedShadow = Request.Source->ImportedDirectionalShadow != 0 ||
										Request.Source->DirectionalShadowBounds.has_value() ||
										State->HasShadowCaptureRequest(Request.Pipeline, Request.TargetSlot);
			const bool worldSetup =
				Request.Damage.Scene
					? (isolatedShadow || State->PreparedScopes.NeedsWorld(selectedPipeline, world))
					: State->BatchShared;
			dispatched = graph::ExecuteRenderView(
				selectedPipeline->Graph,
				selectedPipeline->Projection,
				runner,
				State->BatchViewIndex,
				State->BatchWorldIndex,
				worldSetup,
				frameSetup
			);
			State->PreparedScopes.Complete(selectedPipeline, world, dispatched && Request.Damage.Scene);
			if (isolatedShadow && dispatched && Request.Damage.Scene)
				State->PreparedScopes.InvalidateWorld(selectedPipeline, world);
			if (dispatched && State->BatchFinal) {
				if (State->BatchPresentationCommand != nullptr) {
					closePass();
					dispatched = State->GpuQueues.PreparePresentation(&Command);
					if (dispatched) TimedCommand = Command;
				}
				if (dispatched)
					dispatched = graph::ExecuteRenderFinal(
						selectedPipeline->Graph, selectedPipeline->Projection, runner
					);
			}
		} else {
			const uint64_t worlds[] = {world};
			dispatched = graph::ExecuteRenderStage(
				selectedPipeline->Graph, selectedPipeline->Projection, runner, worlds
			);
		}
		State->DroppedProfileMarks += frameRunner.DroppedProfileMarks();
		if (!dispatched) {
			const core::Name failed =
				frameRunner.Rejected().IsValid() ? frameRunner.Rejected() : frameRunner.Unhandled();
			ENGINE_ERROR(
				"render graph '{}' refused while executing '{}'", selectedPipeline->Name.Text(), failed.Text()
			);
			closePass();
			State->BatchFailed = State->BatchActive;
			endIncompleteView();
			return;
		}

		auto &pending = State->SlotAt(targetSlot).ForwardWorking;
		pending.Activity = Activity;
		pending.Pipeline = Pipeline->Name;
		pending.Revision = Pipeline->Revision;
		pending.World = Request.World;
		pending.ContentSignature = ContentSignature;
		pending.Ready = true;
		pending.Command = GpuCommandIdentity(command);

		closePass();

		// Byte and two-byte masks are expanded to RGBA by the readback path. Float
		// targets remain directly inspectable on the GPU without pretending their
		// bytes are display colour.
		if (offscreen && State->Inspected.IsValid() &&
			(State->InspectedSlot == Renderer::ANY_VIEWPORT || State->InspectedSlot == targetSlot)) {
			Impl::NamedTexture inspected;
			for (uint32_t value = 1; value <= Pipeline->Graph.ResourceCount(); ++value) {
				const graph::ResourceId resource{value};
				const auto *desc = Pipeline->Graph.FindResourceDesc(resource);
				if (desc && desc->Name == State->Inspected) {
					inspected = ResourceTexture(resource, targetSlot, false);
					break;
				}
			}
			if (!inspected.IsValid()) inspected = fixedTexture(State->Inspected, targetSlot);
			if (!inspected.IsValid()) {
				for (const Impl::GraphTarget &target : State->GraphResources.Targets) {
					if (target.Pipeline == selectedPipeline->Name && target.Resource == State->Inspected &&
						target.Texture != nullptr &&
						(target.Scope != graph::NodeScope::View || target.Owner == targetSlot)) {
						inspected = {target.Texture, target.Width, target.Height, target.Format};
						break;
					}
				}
			}
			if (inspected.IsValid()) {
				(void)State->RequestPreview(
					State->DownloadBuffer(),
					inspected.Texture,
					inspected.Width,
					inspected.Height,
					State->Inspected,
					targetSlot,
					inspected.Format
				);
			}
		}

		// A swapchain image is write-only. On a requested Studio screenshot
		// frame the host overlay is therefore recorded into a readable image of
		// the same size, then blitted to the swapchain below. Ordinary frames keep
		// the direct path and pay no extra image or blit.
		const bool windowCaptureRequested =
			present && swapchain != nullptr && drawHostOverlay && !State->WindowCapturePath.empty();
		const bool windowCaptureReady = windowCaptureRequested && State->EnsureWindowCapture(width, height);
		if (windowCaptureReady) {
			windowTarget.texture = State->WindowCaptureTexture;
			windowTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		}

		// --- the capture ----------------------------------------------------
		//
		// After the world's passes and before the window's, because what is
		// wanted is the scene as it was drawn rather than the scene with the
		// editor's panels over it. A copy pass, so it cannot be inside one of
		// the render passes above.
		// Owned consumers use the same completed scene texture and download stream
		// as file captures. Earlier views retain their transfers until the final submit.
		Impl::OwnedSceneCapture *fileCapture = nullptr;
		for (auto &owned : State->SceneCaptures) {
			if (owned.Result.Token == 0 || owned.Complete || owned.Transfer != nullptr ||
				(owned.Request.ViewSlot != Renderer::ANY_VIEWPORT && owned.Request.ViewSlot != targetSlot))
				continue;
			owned.Complete = true;
			if (!offscreen || Request.Source->WorldName.Text().size() > 4096 ||
				Request.Source->SnapshotId.size() > 4096 ||
				(owned.Request.ExpectedSnapshotId &&
				 *owned.Request.ExpectedSnapshotId != Request.Source->SnapshotId))
				continue;
			const uint64_t bytes = static_cast<uint64_t>(sceneWidth) * sceneHeight * 4;
			size_t retainedBytes = 0;
			for (const auto &held : State->SceneCaptures) {
				retainedBytes += held.Transfer != nullptr
									 ? static_cast<size_t>(held.Result.Width) * held.Result.Height * 4
									 : held.Result.Pixels.size();
			}
			if (bytes > owned.Request.MaxBytes || bytes > Impl::SCENE_CAPTURE_BYTE_LIMIT - retainedBytes ||
				bytes > std::numeric_limits<uint32_t>::max()) {
				owned.Result.Status = SceneCaptureStatus::OverBudget;
				continue;
			}
			owned.Result.Frame = State->FrameCounter;
			owned.Result.World = Request.World;
			owned.Result.WorldName = Request.Source->WorldName.Text();
			owned.Result.SnapshotId = Request.Source->SnapshotId;
			owned.Result.ViewSlot = targetSlot;
			owned.Result.CameraFrame = Request.CameraFrame;
			owned.Result.Camera = DrawCamera;
			owned.Result.Projection = Matrices.Projection;
			owned.Result.Pipeline = Pipeline->Name;
			owned.Result.PipelineRevision = Pipeline->Revision;
			owned.Result.Width = sceneWidth;
			owned.Result.Height = sceneHeight;
			owned.Format = State->ColourFormat();
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = static_cast<uint32_t>(bytes);
			owned.Transfer = gpu::CreateTransferBuffer(State->Device, &info);
			SDL_GPUCommandBuffer *downloads = owned.Transfer ? State->DownloadBuffer() : nullptr;
			SDL_GPUCopyPass *copy = downloads ? SDL_BeginGPUCopyPass(downloads) : nullptr;
			if (!copy) {
				if (owned.Transfer) gpu::ReleaseTransferBuffer(State->Device, owned.Transfer);
				owned.Transfer = nullptr;
				continue;
			}
			SDL_GPUTextureRegion source{};
			source.texture = State->SlotAt(targetSlot).Texture;
			source.w = sceneWidth;
			source.h = sceneHeight;
			source.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = owned.Transfer;
			destination.pixels_per_row = sceneWidth;
			destination.rows_per_layer = sceneHeight;
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			core::Metrics::Count("render.scene_capture.download_bytes", bytes);
			core::Metrics::Count("render.scene_capture.downloads", 1);
			owned.Complete = false;
			if (fileCapture == nullptr) fileCapture = &owned;
		}

		SDL_GPUTransferBuffer *capture = nullptr;
		// **And only this viewport's, when one was named.** A request for a
		// panel that is not drawing this call is left pending rather than
		// served with the wrong picture - its turn comes round within as many
		// frames as there are panels.
		const bool captureWantsThis =
			State->CaptureSlot == Renderer::ANY_VIEWPORT || State->CaptureSlot == targetSlot;
		const bool explicitSceneCapture =
			present && offscreen && !State->CapturePath.empty() && captureWantsThis;
		const bool capturingAuthored = authoredCapture.IsValid();
		const bool explicitWindowCapture = !capturingAuthored && !explicitSceneCapture && windowCaptureReady;
		Impl::NamedTexture captureSource = authoredCapture;
		std::filesystem::path capturePath = authoredCapturePath;
		if (!captureSource.IsValid() && explicitSceneCapture) {
			const Impl::SceneSlot &scene = State->SlotAt(targetSlot);
			captureSource = {scene.Texture, sceneWidth, sceneHeight, State->ColourFormat()};
			capturePath = State->CapturePath;
		} else if (!captureSource.IsValid() && explicitWindowCapture) {
			captureSource = {
				State->WindowCaptureTexture,
				State->WindowCaptureWidth,
				State->WindowCaptureHeight,
				State->ColourFormat(),
			};
			capturePath = State->WindowCapturePath;
		}

		if (!explicitSceneCapture || capturingAuthored || fileCapture == nullptr) fileCapture = nullptr;
		if (fileCapture != nullptr) capture = fileCapture->Transfer;

		if (capture == nullptr && captureSource.IsValid() && !capturePath.empty()) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			const uint64_t captureBytes =
				static_cast<uint64_t>(captureSource.Width) * captureSource.Height * sizeof(uint32_t);
			if (captureBytes > std::numeric_limits<uint32_t>::max()) {
				ENGINE_ERROR("capture: {}x{} is too large", captureSource.Width, captureSource.Height);
			} else {
				info.size = static_cast<uint32_t>(captureBytes);
			}

			capture = info.size > 0 ? gpu::CreateTransferBuffer(State->Device, &info) : nullptr;
			if (capture == nullptr) {
				ENGINE_ERROR("capture: SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
			}

			// The copy records on the later-transfer buffer, which is submitted
			// after the main buffer - so the picture is the frame as drawn, on
			// the same one-queue ordering the previews rely on.
			SDL_GPUCommandBuffer *downloads = capture != nullptr ? State->DownloadBuffer() : nullptr;
			if (capture != nullptr && downloads == nullptr) {
				gpu::ReleaseTransferBuffer(State->Device, capture);
				capture = nullptr;
			}
			if (capture != nullptr) {
				SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(downloads);

				SDL_GPUTextureRegion source{};
				source.texture = captureSource.Texture;
				source.w = captureSource.Width;
				source.h = captureSource.Height;
				source.d = 1;

				SDL_GPUTextureTransferInfo destination{};
				destination.transfer_buffer = capture;
				destination.pixels_per_row = captureSource.Width;
				destination.rows_per_layer = captureSource.Height;

				SDL_DownloadFromGPUTexture(copy, &source, &destination);
				SDL_EndGPUCopyPass(copy);
				core::Metrics::Count("render.scene_capture.download_bytes", captureBytes);
				core::Metrics::Count("render.scene_capture.downloads", 1);
			}
		}

		// Keep the interface-facing image separate from the target being written.
		// Its fence is attached when the batch submits below, and the CPU publishes
		// it on a later frame only after that fence signals.
		if (Request.Source->SpatialSnapshotSequence != 0) {
			if (!Request.Damage.SceneImage())
				result.SpatialSnapshot =
					UploadOverlay ? SpatialSnapshotStatus::Unsupported : SpatialSnapshotStatus::Unchanged;
			else if (SnapshotLayerStatus == OverlayRecordStatus::Failed ||
					 (snapshotSawInterface && snapshotScreenFailed) ||
					 (snapshotSawOutput && snapshotOutputFailed) ||
					 (Request.Source->CaptureSpatialDepth && snapshotSawTransparent &&
					  ((!SpatialDepthRecorded &&
						(DrawInterface || SnapshotPreparationStatus != OverlayRecordStatus::Unknown)) ||
					   SpatialWorldStatus == OverlayRecordStatus::Failed)))
				result.SpatialSnapshot = SpatialSnapshotStatus::Unavailable;
			else if (SnapshotLayerStatus == OverlayRecordStatus::Unknown || snapshotOverflow ||
					 !snapshotSawInterface || !snapshotSawOutput || !snapshotProven)
				result.SpatialSnapshot = SpatialSnapshotStatus::Unsupported;
			else {
				result.SpatialSnapshot = SpatialSnapshotStatus::Ready;
				result.SpatialSnapshotSequence = Request.Source->SpatialSnapshotSequence;
				if (SpatialDepthRecorded) {
					result.SpatialDepth = SpatialDepth;
					result.SpatialDepthProjection = SpatialDepthProjection;
				}
			}
		}
		if (offscreen) {
			if (!State->RetainSceneFrame(command, targetSlot, result)) {
				result.SpatialDepth = {};
				result.SpatialDepthProjection.reset();
				result.SpatialSnapshotSequence = 0;
				if (result.SpatialSnapshot == SpatialSnapshotStatus::Ready)
					result.SpatialSnapshot = SpatialSnapshotStatus::Unavailable;
			}
		} else if (!present) {
			result.SpatialDepth = {};
			result.SpatialDepthProjection.reset();
			result.SpatialSnapshotSequence = 0;
		}
		for (auto &staged : State->GraphResources.StagedSpatialDepths) {
			if (staged.Key == SpatialDepth) staged.Publishable = result.SpatialDepth.Frame != 0;
		}

		// Studio chrome is a host concern, not a universe render-graph stage. It
		// is recorded after `output-image`, so graph previews, authored captures,
		// and rendering profiles contain only the game image and game interface.
		if (drawHostOverlay) {
			ENGINE_PROFILE_CAT("host overlay pass", core::ProfileCategory::Render);
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			hostOverlayHook->Record(command, pass);
			SDL_EndGPURenderPass(pass);
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;
			result.DrawCalls++;
		}

		if (windowCaptureReady) {
			SDL_GPUBlitInfo blit{};
			blit.source.texture = State->WindowCaptureTexture;
			blit.source.w = State->WindowCaptureWidth;
			blit.source.h = State->WindowCaptureHeight;
			blit.destination.texture = swapchain;
			blit.destination.w = width;
			blit.destination.h = height;
			blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
			blit.filter = SDL_GPU_FILTER_NEAREST;
			SDL_BlitGPUTexture(command, &blit);
		}

		// **The window, when nothing else touched it.** With the world drawn
		// offscreen and neither the overlay nor the interface open, no pass has
		// reached the swapchain - and presenting a texture the driver handed
		// back without writing to it shows last frame's image or uninitialised
		// memory. One clear costs nothing and removes the whole case.
		if (swapchain != nullptr && windowTarget.load_op == SDL_GPU_LOADOP_CLEAR) {
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			SDL_EndGPURenderPass(pass);
		}

		// Every earlier view leaves command ownership with FrameBatch. The last
		// view submits it, then hands the terminal result back for shared cleanup.
		if (State->BatchActive && !State->BatchFinal) {
			return;
		}

		// Before the submit rather than after it, so a frame that fails to
		// submit still reports what it built.

		{
			// Hands the whole buffer over and queues the present. The passes
			// above only *record* commands, so almost nothing that happens in
			// them is measured by their spans - this is where the driver gets
			// the work, and where any cost of building it lands.
			ENGINE_PROFILE_CAT("submit", core::ProfileCategory::Render);

			// The main buffer goes first: its submit queues the present, and on
			// SDL's one unified queue it is what guarantees every pass has
			// executed before the later-transfer buffer's downloads read their
			// textures. The fences below therefore move to that second buffer.
			bool sceneSubmitted = false;
			{
				ENGINE_PROFILE_CAT("submit.scene", core::ProfileCategory::Render);
				sceneSubmitted = State->BatchActive ? State->SubmitFrameBatchCommand(command)
													: State->SubmitSceneCommand(command);
			}
			const bool batchOwned = State->BatchActive;
			if (batchOwned) {
				State->BatchSubmit = {
					command,
					sceneSubmitted ? Impl::BatchSubmitStatus::Submitted : Impl::BatchSubmitStatus::Failed,
				};
			}
			result.Submitted = sceneSubmitted;
			if (!sceneSubmitted) {
				if (!batchOwned) {
					State->VisibilityWorking.Invalidate();
					State->VisibilityCompleted = {};
					State->StageProbe.Clear(State->Device);
					ENGINE_ERROR("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
					State->CompleteResidentUploads(false);
					State->DiscardPendingGraphHistoryWrites(command);
					State->ClearSubmittedGraphHistoryWrites();
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
						State->AbandonCaptureTimings(timingSlot);
					}
				}
				if (capture != nullptr && fileCapture == nullptr) {
					gpu::ReleaseTransferBuffer(State->Device, capture);
				}
				if (!batchOwned) State->DropDownloads();
				return;
			}
			if (!batchOwned) {
				State->VisibilityCompleted = State->VisibilityWorking.Snapshot();
				ENGINE_PROFILE_CAT("submit.residency complete", core::ProfileCategory::Render);
				State->CompleteResidentUploads(true);
				State->CommitPendingGraphHistoryWrites(command);
				State->ClearSubmittedGraphHistoryWrites();
			}

			if (SDL_GPUCommandBuffer *downloads = State->DownloadCommand; downloads != nullptr) {
				ENGINE_PROFILE_CAT("submit.downloads", core::ProfileCategory::Render);
				State->DownloadCommand = nullptr;
				result.DownloadCommandBuffers++;
				if (capture != nullptr || State->HasSceneCaptureTransfers()) {
					// **A fence, and the stall is the point.** The pixels are
					// not there until the GPU has run the copy, so a capture has
					// to wait for it. That is a frame's worth of latency on the
					// frames a caller asked to capture and on no others. The
					// main buffer is already on the queue, so a failure here
					// loses this frame's downloads, never the frame itself.
					SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(downloads);
					if (fence == nullptr) {
						ENGINE_ERROR("SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError());
						if (capture != nullptr && fileCapture == nullptr)
							gpu::ReleaseTransferBuffer(State->Device, capture);
						State->CompleteSceneCaptures(false);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->Preview.Pixels.clear();
							State->Preview.Histogram = ImageHistogram{};
						}
					} else {
						const bool captureCompleted = SDL_WaitForGPUFences(State->Device, true, &fence, 1);
						State->CompleteSceneCaptures(captureCompleted);
						SDL_ReleaseGPUFence(State->Device, fence);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->CollectPreview();
						}

						const bool wroteCapture =
							captureCompleted && !capturePath.empty() &&
							(fileCapture != nullptr
								 ? fileCapture->Result.Status == SceneCaptureStatus::Ready &&
									   State->WriteCapture(fileCapture->Result, capturePath)
								 : capture != nullptr && State->WriteCapture(
															 capture,
															 captureSource.Width,
															 captureSource.Height,
															 captureSource.Format,
															 capturePath
														 ));
						if (wroteCapture) {
							ENGINE_INFO(
								"captured {} x {} to {}",
								captureSource.Width,
								captureSource.Height,
								capturePath.string()
							);
						}

						if (capture != nullptr && fileCapture == nullptr)
							gpu::ReleaseTransferBuffer(State->Device, capture);

						// Once. A request that repeated would write a file
						// every frame and stall every one of them.
						if (explicitSceneCapture && !capturingAuthored) {
							State->CapturePath.clear();
							State->CaptureSlot = Renderer::ANY_VIEWPORT;
						}
						if (explicitWindowCapture) {
							State->WindowCapturePath.clear();
							gpu::ReleaseTexture(State->Device, State->WindowCaptureTexture);
							State->WindowCaptureTexture = nullptr;
							State->WindowCaptureWidth = 0;
							State->WindowCaptureHeight = 0;
						}
					}
				} else if (State->PreviewSubmitted) {
					State->Preview.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(downloads);
					if (State->Preview.Fence == nullptr) {
						ENGINE_ERROR(
							"resource preview: SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError()
						);
						State->Preview.Pending.Poll(true);
						State->Preview.Pixels.clear();
						State->Preview.Histogram = ImageHistogram{};
					}
				} else if (!SDL_SubmitGPUCommandBuffer(downloads)) {
					ENGINE_ERROR("SDL_SubmitGPUCommandBuffer (downloads): {}", SDL_GetError());
				}
			}
		}
		if (timingSlot < VulkanTimestamps::SLOTS) {
			State->Timestamps.Submitted(timingSlot);
			if (!State->PendingMarks[timingSlot].empty()) {
				State->TimingSequence[timingSlot] = State->NextTimingSequence++;
			} else {
				State->TimingSequence[timingSlot] = 0;
			}
		}

		// **Not presented, because there is nowhere to present to.** A caller
		// counting presented frames gets zero from a headless renderer, which is
		// the honest answer - what it should count instead is captures, or its
		// own loop.
		result.Presented = swapchain != nullptr;
		return;
	}

}
