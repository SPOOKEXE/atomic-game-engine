// What a scene pass is, wherever it is opened - and the rest of the operations
// every node family shares.
//
// **These were lambdas inside `Renderer::RenderView`, which is the whole reason
// the passes had to live there too.** A handler that needed `openScenePass` had
// to be written in the same function as it. As member functions they are
// callable from any of the node families, and the families are separate
// translation units, which is what took the module off one compile.
//
// The three capture passes stay separate on purpose - a mirror reflects its
// viewer through one plane and a hole carries it onto a second pane, and the
// two panes read their pictures back by different lookups. What was never a
// difference between them is how a scene pass is *opened* and what the world's
// own draws look like once it is, and those were written out twice side by side
// before v0.15, so every bug in one was available to the other.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace engine::render {

	const graph::Node *ViewRecording::GraphNode(core::Name kind) const {
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;

		for (size_t index = 0; index < selectedPipeline->Graph.Count(); index++) {
			const graph::Node *node =
				selectedPipeline->Graph.Find(graph::NodeId{static_cast<uint32_t>(index + 1)});
			if (node != nullptr && node->Enabled && node->Kind == kind) {
				return node;
			}
		}
		return nullptr;
	}

	bool ViewRecording::GraphEnabled(core::Name kind) const {
		return GraphNode(kind) != nullptr;
	}

	const graph::ScheduledNode *ViewRecording::ScheduledFor(graph::NodeId id) const {
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;

		for (const graph::ExecutionWave &wave : selectedPipeline->Schedule.Waves) {
			for (const graph::ScheduledNode &scheduled : wave.Nodes) {
				if (scheduled.Node == id) {
					return &scheduled;
				}
			}
		}
		return nullptr;
	}

	void ViewRecording::EndIncompleteView() {
		Impl *const State = this->State;
		SDL_GPUCommandBuffer *const command = Command;

		if (State->BatchActive) {
			// The batch owner drops any recorded downloads with the frame.
			State->BatchFailed = true;
		} else {
			State->CompleteResidentUploads(SDL_SubmitGPUCommandBuffer(command));
			State->DropDownloads();
		}
	}

	void ViewRecording::ClosePass() {
		Impl *const State = this->State;
		core::Name &timedName = TimedName;
		const uint32_t timingSlot = TimingSlot;
		SDL_GPUCommandBuffer *const timedCommand = TimedCommand;
		const uint32_t openedMark = OpenedMark;
		const auto openedWall = OpenedWall;

		if (!timedName.IsValid()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		State->WallTimings[timedName.Id()] +=
			std::chrono::duration<double, std::micro>(now - openedWall).count();
		if (timingSlot < VulkanTimestamps::SLOTS) {
			const uint32_t closedMark = State->Timestamps.Mark(timedCommand);
			if (openedMark < VulkanTimestamps::MARKS && closedMark < VulkanTimestamps::MARKS) {
				State->PendingMarks[timingSlot].push_back({timedName, openedMark, closedMark});
			}
		}
		timedName = {};
	}

	void ViewRecording::EnterNamedPass(core::Name name, SDL_GPUCommandBuffer *recordedCommand) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;
		SDL_GPUCommandBuffer *const command = Command;
		core::Name &timedName = TimedName;
		const uint32_t timingSlot = TimingSlot;
		SDL_GPUCommandBuffer *&timedCommand = TimedCommand;
		uint32_t &openedMark = OpenedMark;
		auto &openedWall = OpenedWall;
		bool &mainGpuWorkRecorded = MainGpuWorkRecorded;

		ClosePass();
		timedName = name;
		timedCommand = recordedCommand != nullptr ? recordedCommand : command;
		const graph::Node *node = nullptr;
		const graph::ScheduledNode *scheduled = nullptr;
		for (const graph::ExecutionWave &wave : selectedPipeline->Schedule.Waves) {
			for (const graph::ScheduledNode &candidate : wave.Nodes) {
				const graph::Node *candidateNode = selectedPipeline->Graph.Find(candidate.Node);
				if (candidateNode != nullptr && candidateNode->Name == name) {
					node = candidateNode;
					scheduled = &candidate;
					break;
				}
			}
			if (scheduled != nullptr) {
				break;
			}
		}
		if (scheduled != nullptr &&
			(scheduled->Queue == graph::ExecutionQueue::Graphics ||
			 scheduled->Queue == graph::ExecutionQueue::Transfer) &&
			node->Kind != core::Name("upload-instances")) {
			mainGpuWorkRecorded = true;
		}
		if (std::find(result.Nodes.begin(), result.Nodes.end(), name) == result.Nodes.end()) {
			result.Nodes.push_back(name);
		}
		openedWall = std::chrono::steady_clock::now();
		openedMark = timingSlot < VulkanTimestamps::SLOTS ? State->Timestamps.Mark(timedCommand)
														  : VulkanTimestamps::MARKS;
	}

	bool ViewRecording::FinishCpuNode(const graph::RunContext &context) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		const auto &cpuNodeWall = CpuNodeWall;

		ClosePass();
		if (std::find(result.Nodes.begin(), result.Nodes.end(), context.Name) == result.Nodes.end()) {
			result.Nodes.push_back(context.Name);
		}
		if (const auto found = cpuNodeWall.find(context.Name.Id()); found != cpuNodeWall.end()) {
			State->WallTimings[context.Name.Id()] += found->second;
		}
		return true;
	}

	bool ViewRecording::RecordUploads() {
		Impl *const State = this->State;
		FrameResult &result = Result;
		OverlayImage &overlay = *Request.Overlay;
		const bool haveInstances = HaveInstances;
		const bool uploadOverlay = UploadOverlay;
		const uint32_t particleCount = ParticleCount;
		const uint32_t ribbonCount = RibbonCount;
		bool &uploadsRecorded = UploadsRecorded;

		if (uploadsRecorded) {
			return true;
		}

		Impl::SceneSlot &target = State->SlotAt(State->ActiveSlot);
		const bool uploadInstances =
			haveInstances && (State->ActiveInstanceWorld == nullptr ||
							  State->ActiveInstanceWorld->Instances.DirtyCount() > 0 ||
							  target.ResidentIndices.DirtyCount() > 0 || State->OcclusionFrame.Active);
		if (!uploadInstances && !uploadOverlay && particleCount == 0 && ribbonCount == 0) {
			uploadsRecorded = true;
			return true;
		}

		// **Past the early-out, so it names work rather than a null check.**
		// Multiple `SDL_UploadToGPUBuffer` calls and a transfer-buffer map all
		// live inside whichever graph node first needs them, so keep the whole
		// record phase visible rather than timing only overlay staging.
		ENGINE_PROFILE_CAT("record uploads", core::ProfileCategory::Render);

		OverlayImage::Region overlayRegion;
		if (uploadOverlay) {
			ENGINE_PROFILE_CAT("stage overlay", core::ProfileCategory::Render);
			overlayRegion = State->OverlayUninitialised
								? OverlayImage::Region{0, 0, overlay.GetWidth(), overlay.GetHeight()}
								: overlay.UploadRegion();
			const auto rowBytes = static_cast<size_t>(overlayRegion.Width) * OverlayImage::BYTES_PER_PIXEL;
			void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->OverlayTransfer, true);
			if (mapped == nullptr) {
				ENGINE_ERROR("upload overlay: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
				return false;
			}

			auto *destination = static_cast<uint8_t *>(mapped);
			const uint8_t *pixels = overlay.GetPixels();
			const auto stride = static_cast<size_t>(overlay.GetWidth()) * OverlayImage::BYTES_PER_PIXEL;
			for (int row = 0; row < overlayRegion.Height; row++) {
				const size_t offset = static_cast<size_t>(overlayRegion.Y + row) * stride +
									  static_cast<size_t>(overlayRegion.X) * OverlayImage::BYTES_PER_PIXEL;
				std::memcpy(destination + static_cast<size_t>(row) * rowBytes, pixels + offset, rowBytes);
			}
			SDL_UnmapGPUTransferBuffer(State->Device, State->OverlayTransfer);
		}

		SDL_GPUCommandBuffer *const uploadCommand = Command;
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(uploadCommand);
		if (copy == nullptr) {
			ENGINE_ERROR("upload instances: SDL_BeginGPUCopyPass: {}", SDL_GetError());
			return false;
		}

		uint64_t uploadedBytes = 0;
		if (uploadInstances) {
			Impl::InstanceWorld *const world = State->ActiveInstanceWorld;
			if (world == nullptr) {
				SDL_EndGPUCopyPass(copy);
				return false;
			}
			for (const InstanceUploadRange &range : world->Instances.DirtyRanges()) {
				const uint32_t offset = range.First * static_cast<uint32_t>(sizeof(GpuInstance));
				const SDL_GPUTransferBufferLocation source{State->InstanceTransfer, offset};
				const SDL_GPUBufferRegion destination{
					State->InstanceBuffer,
					offset,
					range.Count * static_cast<uint32_t>(sizeof(GpuInstance)),
				};
				// Queue order protects unchanged resident rows. Cycling here would
				// select fresh storage and discard every row this partial copy omits.
				SDL_UploadToGPUBuffer(copy, &source, &destination, false);
				uploadedBytes += destination.size;
			}

			for (const InstanceUploadRange &range : target.ResidentIndices.DirtyRanges()) {
				const uint32_t offset = range.First * static_cast<uint32_t>(sizeof(uint32_t));
				const SDL_GPUTransferBufferLocation source{State->InstanceIndexTransfer, offset};
				const SDL_GPUBufferRegion destination{
					State->InstanceIndexBuffer,
					offset,
					range.Count * static_cast<uint32_t>(sizeof(uint32_t)),
				};
				SDL_UploadToGPUBuffer(copy, &source, &destination, false);
				uploadedBytes += destination.size;
			}
		}

		// The occlusion plan's five buffers, in the order its staging wrote
		// them. Cycled like the instances: this copy is each buffer's first
		// touch of the frame, so a later view gets a fresh version while
		// the previous view's dispatches keep the one they bound. The cull
		// pass itself must *not* cycle `Counts` again - its atomics count
		// up from the zeros this copy delivers.
		if (uploadInstances && State->OcclusionFrame.Active) {
			const Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;
			uint32_t offset = 0;
			const auto stage = [&](SDL_GPUBuffer *buffer, uint32_t bytes) {
				const SDL_GPUTransferBufferLocation source{State->Occlusion.Transfer, offset};
				const SDL_GPUBufferRegion destination{buffer, 0, bytes};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
				offset += bytes;
				uploadedBytes += bytes;
			};
			stage(
				State->Occlusion.Arguments,
				occlusionPlan.ArgCount * 2 * static_cast<uint32_t>(sizeof(SDL_GPUIndexedIndirectDrawCommand))
			);
			stage(
				State->Occlusion.Candidates,
				occlusionPlan.CandidateCount * 2 * static_cast<uint32_t>(sizeof(glm::vec4))
			);
			stage(State->Occlusion.RunTable, occlusionPlan.RunCount * sizeof(uint32_t));
			stage(State->Occlusion.ArgRuns, occlusionPlan.ArgCount * sizeof(uint32_t));
			stage(State->Occlusion.Counts, occlusionPlan.RunCount * sizeof(uint32_t));
		}

		if (ribbonCount > 0) {
			const SDL_GPUTransferBufferLocation source{State->RibbonTransfer, 0};
			const SDL_GPUBufferRegion destination{
				State->RibbonBuffer,
				0,
				ribbonCount * static_cast<uint32_t>(sizeof(effects::RibbonVertex)),
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			uploadedBytes += destination.size;
		}

		// **No particle instance upload here.** The instance stream is not host
		// data that has to cross: `PrepareParticles` recorded changed block data
		// and `particle-step.comp` into this command before the graph passes.
		// Queue order makes this later draw see the generated rows.

		if (uploadOverlay) {
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = State->OverlayTransfer;
			source.pixels_per_row = static_cast<uint32_t>(overlayRegion.Width);
			source.rows_per_layer = static_cast<uint32_t>(overlayRegion.Height);

			SDL_GPUTextureRegion destination{};
			destination.texture = State->OverlayTexture;
			destination.x = static_cast<uint32_t>(overlayRegion.X);
			destination.y = static_cast<uint32_t>(overlayRegion.Y);
			destination.w = static_cast<uint32_t>(overlayRegion.Width);
			destination.h = static_cast<uint32_t>(overlayRegion.Height);
			destination.d = 1;

			// Keep the texture allocation because a partial upload must preserve
			// every pixel outside the dirty rectangle.
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
			uploadedBytes += static_cast<uint64_t>(overlayRegion.Width) * overlayRegion.Height *
							 OverlayImage::BYTES_PER_PIXEL;
		}

		SDL_EndGPUCopyPass(copy);
		if (uploadInstances && State->ActiveInstanceWorld != nullptr &&
			State->ActiveInstanceWorld->Instances.DirtyCount() > 0) {
			State->TrackInstanceUpload(*State->ActiveInstanceWorld);
			State->ActiveInstanceWorld->Instances.AcknowledgeDirty();
		}
		if (uploadInstances && target.ResidentIndices.DirtyCount() > 0) {
			target.ResidentIndices.Acknowledge();
		}

		// The copy is in the frame's main command buffer, before every draw that
		// reads it. This keeps a render batch one submission and makes the graph's
		// GPU timestamps measure the transfer rather than two adjacent marks in a
		// different command buffer. Resource cycling still gives each later view
		// a fresh target-local index stream.
		if (uploadOverlay) {
			State->OverlayUninitialised = false;
			overlay.MarkUploaded();
		}
		result.UploadedBytes += uploadedBytes;
		uploadsRecorded = true;
		return true;
	}

	LightingUniforms ViewRecording::LightingFrom(
		const scene::WorldLighting &worldLighting,
		const core::Vector3 &eye,
		float surfaceMode,
		float imageOpacity
	) const {
		const bool haveShadow = HaveShadow;

		LightingUniforms lighting;
		lighting.Direction = glm::vec4{
			worldLighting.Direction.X,
			worldLighting.Direction.Y,
			worldLighting.Direction.Z,
			0.0f,
		};
		lighting.Ambient = glm::vec4{
			worldLighting.Ambient.R,
			worldLighting.Ambient.G,
			worldLighting.Ambient.B,
			1.0f,
		};
		lighting.Direct = glm::vec4{
			worldLighting.Direct.R,
			worldLighting.Direct.G,
			worldLighting.Direct.B,
			1.0f,
		};
		lighting.Flags = glm::vec4{
			haveShadow ? 1.0f : 0.0f,
			1.0f / static_cast<float>(SHADOW_RESOLUTION),
			surfaceMode,
			imageOpacity,
		};
		lighting.OutdoorAmbient = glm::vec4{
			worldLighting.OutdoorAmbient.R,
			worldLighting.OutdoorAmbient.G,
			worldLighting.OutdoorAmbient.B,
			1.0f,
		};
		lighting.FogColour = glm::vec4{
			worldLighting.FogColor.R,
			worldLighting.FogColor.G,
			worldLighting.FogColor.B,
			1.0f,
		};
		lighting.Fog = glm::vec4{worldLighting.FogStart, worldLighting.FogEnd, 0.0f, 0.0f};
		lighting.Eye = glm::vec4{eye.X, eye.Y, eye.Z, 0.0f};
		return lighting;
	}

	LightingUniforms
	ViewRecording::LightingAt(const core::Vector3 &eye, float surfaceMode, float imageOpacity) const {
		return LightingFrom(CurrentLighting, eye, surfaceMode, imageOpacity);
	}

	ShadowBinding ViewRecording::ShadowBindings() const {
		Impl *const State = this->State;

		return ShadowBinding{
			State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture,
			State->ShadowSampler != nullptr ? State->ShadowSampler : State->SurfaceSampler,
		};
	}

	SDL_GPURenderPass *ViewRecording::OpenScenePass(
		SDL_GPUTexture *colour,
		SDL_GPUTexture *depth,
		bool cycle,
		const SDL_GPUViewport *viewport,
		const LightUniforms &passLights,
		const SDL_FColor *clearColour
	) {
		Impl *const State = this->State;
		SDL_GPUCommandBuffer *const command = Command;

		SDL_GPUColorTargetInfo colourInfo{};
		colourInfo.texture = colour;
		colourInfo.clear_color = clearColour != nullptr ? *clearColour
														: SDL_FColor{
															  State->FogColour.r,
															  State->FogColour.g,
															  State->FogColour.b,
															  1.0f,
														  };
		colourInfo.load_op = SDL_GPU_LOADOP_CLEAR;
		colourInfo.store_op = SDL_GPU_STOREOP_STORE;
		colourInfo.cycle = cycle;

		SDL_GPUDepthStencilTargetInfo depthInfo{};
		depthInfo.texture = depth;
		depthInfo.clear_depth = 1.0f;
		depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
		depthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthInfo.cycle = cycle;

		SDL_GPURenderPass *const pass = SDL_BeginGPURenderPass(command, &colourInfo, 1, &depthInfo);

		if (viewport != nullptr) {
			SDL_SetGPUViewport(pass, viewport);

			const SDL_Rect scissor{
				static_cast<int>(viewport->x),
				static_cast<int>(viewport->y),
				static_cast<int>(viewport->w),
				static_cast<int>(viewport->h)
			};
			SDL_SetGPUScissor(pass, &scissor);
		}

		// **The light set, pushed once for the whole pass.** Uniform state
		// on a command buffer persists until it is replaced, so one push
		// before the draws serves every one of them - which is the whole
		// reason this is a second buffer rather than fields on the per-draw
		// `LightingUniforms`.
		SDL_PushGPUFragmentUniformData(command, 1, &passLights, sizeof(passLights));

		// **The beams, beside the lights and for the same reason.** Which
		// holes carry a shadow is a fact about the frame, so it is pushed
		// once per pass rather than per draw - and it is pushed even when
		// there are none, because a stale block from a previous frame would
		// shadow through a hole that is no longer there.
		SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
		State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);

		State->BindInstanceBuffers(pass);

		const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

		return pass;
	}

	void ViewRecording::DrawWorldInto(
		SDL_GPURenderPass *pass, const LightingUniforms &plainLighting, uint32_t filter
	) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		SDL_GPUCommandBuffer *const command = Command;
		const uint32_t sceneReflected = SceneReflected;

		if (sceneReflected > 0) {
			const ShadowBinding shadow = ShadowBindings();
			result.DrawCalls += State->DrawSlots(
				command,
				pass,
				0,
				sceneReflected,
				&plainLighting,
				shadow.Texture,
				shadow.Sampler,
				nullptr,
				State->SurfaceSampler,
				filter,
				result.Triangles
			);
		}
	}

	void ViewRecording::DrawBlendedInto(
		SDL_GPURenderPass *pass,
		const FrameUniforms &frame,
		const LightingUniforms &plainLighting,
		uint32_t filter,
		bool panesFollow
	) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		SDL_GPUCommandBuffer *const command = Command;
		const scene::ScenePlan &plan = Plan;
		const size_t sceneOpaque = SceneOpaque;
		const uint32_t sceneTransparent = SceneTransparent;

		const uint32_t blendedPlain = sceneTransparent - plan.TransparentSurfaces;
		if (blendedPlain > 0 || (panesFollow && plan.TransparentSurfaces > 0)) {
			State->BindPipeline(pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent);
		}

		if (blendedPlain == 0) {
			return;
		}

		SDL_PushGPUVertexUniformData(command, 0, &frame, sizeof(FrameUniforms));

		const ShadowBinding shadow = ShadowBindings();
		result.DrawCalls += State->DrawSlots(
			command,
			pass,
			static_cast<uint32_t>(sceneOpaque),
			blendedPlain,
			&plainLighting,
			shadow.Texture,
			shadow.Sampler,
			nullptr,
			State->SurfaceSampler,
			filter,
			result.Triangles
		);
	}

	void ViewRecording::Fullscreen(
		core::Name name,
		SDL_GPUGraphicsPipeline *pipeline,
		SDL_GPUTexture *target,
		uint32_t passWidth,
		uint32_t passHeight,
		std::span<const SDL_GPUTextureSamplerBinding> bindings,
		const PbrUniforms *passUniforms,
		const LightUniforms *passLights,
		SDL_FColor clear
	) {
		FrameResult &result = Result;
		SDL_GPUCommandBuffer *const command = Command;

		EnterNamedPass(name);
		SDL_GPUColorTargetInfo colour{};
		colour.texture = target;
		colour.clear_color = clear;
		colour.load_op = SDL_GPU_LOADOP_CLEAR;
		colour.store_op = SDL_GPU_STOREOP_STORE;
		colour.cycle = true;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		if (!bindings.empty()) {
			SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
		}
		if (passUniforms != nullptr) {
			SDL_PushGPUFragmentUniformData(command, 0, passUniforms, sizeof(*passUniforms));
		}
		if (passLights != nullptr) {
			SDL_PushGPUFragmentUniformData(command, 1, passLights, sizeof(*passLights));
		}
		const SDL_GPUViewport viewport{
			0.0f, 0.0f, static_cast<float>(passWidth), static_cast<float>(passHeight), 0.0f, 1.0f
		};
		const SDL_Rect scissor{0, 0, static_cast<int>(passWidth), static_cast<int>(passHeight)};
		SDL_SetGPUViewport(pass, &viewport);
		SDL_SetGPUScissor(pass, &scissor);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		result.DrawCalls++;
	}

	Renderer::Impl::NamedTexture ViewRecording::FixedTexture(core::Name resource, size_t slot) const {
		Impl *const State = this->State;
		const size_t targetSlot = Request.TargetSlot;
		SDL_GPUTexture *const viewTarget = ViewTarget;
		const uint32_t sceneWidth = SceneWidth;
		const uint32_t sceneHeight = SceneHeight;

		Impl::NamedTexture texture;
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame) {
			if (slot < State->SceneSlots.size()) {
				const Impl::SceneSlot &history = State->SceneSlots[slot];
				return Impl::NamedTexture{
					history.HistoryReady ? history.History : nullptr,
					history.HistoryWidth,
					history.HistoryHeight,
					State->ColourFormat(),
				};
			}
			return texture;
		}
		if (role == Impl::ResourceRole::Scene) {
			if (slot == targetSlot) {
				return Impl::NamedTexture{viewTarget, sceneWidth, sceneHeight, State->ColourFormat()};
			}
			if (slot < State->SceneSlots.size()) {
				const Impl::SceneSlot &scene = State->SceneSlots[slot];
				return Impl::NamedTexture{
					scene.Texture, scene.DrawnWidth, scene.DrawnHeight, State->ColourFormat()
				};
			}
			return texture;
		}
		if (role == Impl::ResourceRole::Depth) {
			if (slot < State->SceneSlots.size()) {
				const Impl::SceneSlot &scene = State->SceneSlots[slot];
				return Impl::NamedTexture{
					scene.Depth, scene.DepthWidth, scene.DepthHeight, State->DepthFormat
				};
			}
			return texture;
		}
		if (role == Impl::ResourceRole::Shadow) {
			return Impl::NamedTexture{
				State->ShadowTexture,
				SHADOW_RESOLUTION,
				SHADOW_RESOLUTION,
				State->DepthFormat,
			};
		}
		if (role == Impl::ResourceRole::Surface && slot < State->SurfaceBanks.size()) {
			const Impl::SurfaceSlotState &surface = State->SurfaceBanks[slot].Surfaces[0];
			return Impl::NamedTexture{
				surface.Ready ? surface.Texture[surface.Slot] : nullptr,
				surface.Width,
				surface.Height,
				State->ColourFormat(),
			};
		}
		if (role == Impl::ResourceRole::PortalImage && slot < State->SurfaceBanks.size()) {
			const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
			for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
				for (const Impl::PortalTarget &portal : level->Targets) {
					SDL_GPUTexture *texture = portal.Colour;
					if (texture == nullptr) {
						continue;
					}
					return Impl::NamedTexture{
						texture,
						portal.Width,
						portal.Height,
						State->ColourFormat(),
					};
				}
			}
		}
		if (role == Impl::ResourceRole::PortalDisplay && slot < State->SurfaceBanks.size()) {
			const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
			for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
				for (const Impl::PortalTarget &portal : level->Targets) {
					if (portal.Display == nullptr) {
						continue;
					}
					return Impl::NamedTexture{
						portal.Display,
						portal.Width,
						portal.Height,
						State->ColourFormat(),
					};
				}
			}
		}
		if (role == Impl::ResourceRole::PortalLight && slot < State->SurfaceBanks.size()) {
			for (const Impl::SeamLightTarget &seamLight : State->SurfaceBanks[slot].SeamLights) {
				if (seamLight.Ready && seamLight.Colour != nullptr) {
					return Impl::NamedTexture{
						seamLight.Colour,
						seamLight.Width,
						seamLight.Height,
						State->ColourFormat(),
					};
				}
			}
		}
		if (slot >= State->PbrSlots.size()) {
			return texture;
		}
		const Impl::PbrSlot &slotPbr = State->PbrSlots[slot];
		if (role == Impl::ResourceRole::Albedo) {
			return Impl::NamedTexture{
				slotPbr.Albedo,
				slotPbr.Dimensions.TargetWidth,
				slotPbr.Dimensions.TargetHeight,
				SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
			};
		}
		if (role == Impl::ResourceRole::Normal) {
			return Impl::NamedTexture{
				slotPbr.Normal,
				slotPbr.Dimensions.TargetWidth,
				slotPbr.Dimensions.TargetHeight,
				SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM,
			};
		}
		if (role == Impl::ResourceRole::Material) {
			return Impl::NamedTexture{
				slotPbr.Material,
				slotPbr.Dimensions.TargetWidth,
				slotPbr.Dimensions.TargetHeight,
				SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			};
		}
		if (role == Impl::ResourceRole::Emissive) {
			return Impl::NamedTexture{
				slotPbr.Emissive,
				slotPbr.Dimensions.TargetWidth,
				slotPbr.Dimensions.TargetHeight,
				SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
			};
		}
		if (role == Impl::ResourceRole::LinearDepth) {
			return Impl::NamedTexture{
				slotPbr.LinearDepth,
				slotPbr.Dimensions.LinearWidth,
				slotPbr.Dimensions.LinearHeight,
				SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
			};
		}
		if (role == Impl::ResourceRole::Occlusion) {
			return Impl::NamedTexture{
				slotPbr.Occlusion,
				slotPbr.Dimensions.OcclusionWidth,
				slotPbr.Dimensions.OcclusionHeight,
				SDL_GPU_TEXTUREFORMAT_R8_UNORM,
			};
		}
		if (role == Impl::ResourceRole::Lit) {
			return Impl::NamedTexture{
				slotPbr.Lit,
				slotPbr.Dimensions.LitWidth,
				slotPbr.Dimensions.LitHeight,
				SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
			};
		}
		return texture;
	}

	Renderer::Impl::NamedTexture
	ViewRecording::ResourceTexture(graph::ResourceId resource, size_t selectedSlot, bool make) {
		Impl *const State = this->State;
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;
		SDL_GPUTexture *const swapchain = Swapchain;
		const uint32_t width = Width;
		const uint32_t height = Height;
		const uint32_t sceneWidth = SceneWidth;
		const uint32_t sceneHeight = SceneHeight;
		const uint64_t world = Request.World;

		const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
		if (desc == nullptr) {
			return Impl::NamedTexture{};
		}
		Impl::NamedTexture fixed = FixedTexture(desc->Name, selectedSlot);
		if (fixed.IsValid()) {
			return fixed;
		}
		const graph::NodeScope scope = State->ResourceScope(*selectedPipeline, resource);
		const uint64_t owner = scope == graph::NodeScope::View	  ? static_cast<uint64_t>(selectedSlot)
							   : scope == graph::NodeScope::World ? world
																  : 0;
		if (desc->External) {
			if (desc->Name == core::Name("window")) {
				return Impl::NamedTexture{swapchain, width, height, State->ColourFormat()};
			}
			if (!make) {
				return State->FindGraphTarget(*selectedPipeline, desc->Name, scope, owner);
			}
		}

		if (make) {
			// Graph images ultimately feed this view's output target. Using the
			// Studio swapchain here makes an offscreen interface draw in one
			// coordinate space while its pixel scissors are applied in another.
			const uint32_t resourceWidth = sceneWidth;
			const uint32_t resourceHeight = sceneHeight;
			return State->EnsureGraphTarget(
				*selectedPipeline, resource, owner, resourceWidth, resourceHeight
			);
		}
		return State->FindGraphTarget(*selectedPipeline, desc->Name, scope, owner);
	}

	Renderer::Impl::NamedTexture
	ViewRecording::GraphTexture(graph::ResourceId resource, const graph::RunContext &context, bool make) {
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;
		const size_t targetSlot = Request.TargetSlot;

		const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
		const bool selectsView = context.View == graph::RunContext::WHOLE_FRAME && node != nullptr &&
								 node->Parameter(core::Name("view")) != nullptr;
		const size_t selectedSlot = selectsView ? node->Integer(core::Name("view"), 0) : targetSlot;
		return ResourceTexture(resource, selectedSlot, make);
	}

	std::vector<SDL_GPUTextureSamplerBinding>
	ViewRecording::TextureBindings(const graph::RunContext &context) {
		Impl *const State = this->State;
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;

		std::vector<SDL_GPUTextureSamplerBinding> bindings;
		for (const graph::ResourceId resource : context.Reads) {
			const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
			if (desc == nullptr || desc->Kind == graph::ResourceKind::Buffer ||
				desc->Kind == graph::ResourceKind::Camera || desc->Kind == graph::ResourceKind::Entities) {
				continue;
			}
			Impl::NamedTexture source = GraphTexture(resource, context, false);
			bindings.push_back(
				SDL_GPUTextureSamplerBinding{
					source.IsValid() ? source.Texture : State->FallbackTexture,
					State->SurfaceSampler,
				}
			);
		}
		return bindings;
	}

	namespace {
		// Whether a texture holds one channel, which is what the image pipeline
		// needs to know to spread it across three rather than sampling green
		// and blue that are not there.
		//
		// @param source The image about to be drawn.
		// @return `true` for the single-channel and depth formats.
		bool singleChannel(const ViewRecording::Impl::NamedTexture &source) {
			switch (source.Format) {
			case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
				return true;
			default:
				return false;
			}
		}
	}

	bool ViewRecording::DrawImage(
		const Impl::NamedTexture &source,
		const Impl::NamedTexture &target,
		SDL_GPULoadOp load,
		bool reverseSpectrum
	) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		SDL_GPUCommandBuffer *const command = Command;

		if (!source.IsValid() || !target.IsValid()) {
			return false;
		}
		SDL_GPUColorTargetInfo colour{};
		colour.texture = target.Texture;
		colour.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
		colour.load_op = load;
		colour.store_op = SDL_GPU_STOREOP_STORE;
		colour.cycle = load == SDL_GPU_LOADOP_CLEAR;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
		State->BindPipeline(pass, State->ImagePipeline, Impl::PipelineFamily::Other);
		const SDL_GPUTextureSamplerBinding binding{source.Texture, State->SurfaceSampler};
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		const ImageUniformMode mode = ImageMode(singleChannel(source), reverseSpectrum);
		SDL_PushGPUFragmentUniformData(command, 0, &mode, sizeof(mode));
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		result.DrawCalls++;
		return true;
	}

	bool ViewRecording::DrawOverlayImage(
		SDL_GPUTexture *source, const Impl::NamedTexture &target, SDL_GPULoadOp load
	) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		SDL_GPUCommandBuffer *const command = Command;

		if (source == nullptr || !target.IsValid()) {
			return false;
		}
		SDL_GPUColorTargetInfo colour{};
		colour.texture = target.Texture;
		colour.load_op = load;
		colour.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
		State->BindPipeline(pass, State->OverlayPipeline, Impl::PipelineFamily::Other);
		const SDL_GPUTextureSamplerBinding binding{source, State->OverlaySampler};
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		result.DrawCalls++;
		return true;
	}

	void ViewRecording::ClearOcclusion() {
		Impl::PbrSlot &pbr = *Pbr;
		SDL_GPUCommandBuffer *const command = Command;

		SDL_GPUColorTargetInfo clearAo{};
		clearAo.texture = pbr.Occlusion;
		clearAo.clear_color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
		clearAo.load_op = SDL_GPU_LOADOP_CLEAR;
		clearAo.store_op = SDL_GPU_STOREOP_STORE;
		clearAo.cycle = true;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &clearAo, 1, nullptr);
		SDL_EndGPURenderPass(pass);
	}
}
