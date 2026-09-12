#pragma once

#include "FramePreparation.hpp"
#include "PortalCaptureTreeWork.hpp"

// `Renderer::Impl` - every device object the renderer owns, and the operations
// over them.
//
// **A header rather than a definition inside `Renderer.cpp`, because a frame is
// recorded by more than one file now.** The node families in `src/nodes/` each
// record a pass against this state, so the state has to be nameable from more
// than one translation unit. It stays private: nothing here is reachable from
// `include/`, and `render/AGENTS.md`'s rule that no SDL header appears in a
// public header is what confines it to `src/`.

#include "GpuHeap.hpp"
#include "IndexResidency.hpp"
#include "InstanceResidency.hpp"
#include "LodSelection.hpp"
#include "ParticleWork.hpp"
#include "RenderStageProbe.hpp"
#include "RenderTypes.hpp"
#include "ResourcePreview.hpp"
#include "ShaderBinary.hpp"
#include "SurfaceCapturePlan.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalImageImport.hpp>
#include <engine/render/PortalShadowImageImport.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/TextureTable.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::render {

	struct Renderer::Impl {
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Device = nullptr;
		RenderStageProbe StageProbe;
		DeviceCaps Caps;

		struct NamedPipeline {
			core::Name Name;
			graph::RenderGraph Graph;
			graph::CompiledGraph Compiled;
			std::vector<graph::NodeId> EntityNodes;
			graph::ExecutionSchedule Schedule;
			graph::ResourceAliasPlan Aliases;

			// The schedule's traffic plan, computed once at install. It decides
			// which command buffer class records each node, and its order is the
			// frame's submission order on SDL's one unified queue.
			std::vector<graph::PlannedCommandBuffer> Buffers;
			uint64_t Revision = 0;
		};

		uint64_t PipelineRevision = 0;
		std::optional<NamedPipeline> EngineDefault;
		std::vector<NamedPipeline> NamedPipelines;
		core::Name ActiveGraph;

		const NamedPipeline *PipelineFor(core::Name name) const {
			if (!name.IsValid()) {
				return EngineDefault ? &*EngineDefault : nullptr;
			}
			for (const NamedPipeline &pipeline : NamedPipelines) {
				if (pipeline.Name == name) {
					return &pipeline;
				}
			}
			return EngineDefault ? &*EngineDefault : nullptr;
		}

		enum class ResourceRole : uint8_t {
			Unknown,
			Scene,
			PreviousFrame,
			Depth,
			Shadow,
			Surface,
			PortalImage,
			PortalDisplay,
			PortalLight,
			Albedo,
			Normal,
			Material,
			Emissive,
			LinearDepth,
			Occlusion,
			Lit,
			SkyLit,
			VolumeLit,
		};

		ResourceRole RoleFor(core::Name resource) const {
			const NamedPipeline *pipeline = PipelineFor(ActiveGraph);
			if (pipeline == nullptr) {
				return ResourceRole::Unknown;
			}
			for (uint32_t value = 1; value <= pipeline->Graph.Count(); value++) {
				const graph::Node *node = pipeline->Graph.Find(graph::NodeId{value});
				if (node == nullptr) {
					continue;
				}
				for (size_t output = 0; output < node->Writes.size(); output++) {
					const graph::ResourceDesc *desc = pipeline->Graph.FindResource(node->Writes[output]);
					if (desc == nullptr || desc->Name != resource) {
						continue;
					}
					if (node->Kind == core::Name("shadow")) {
						return ResourceRole::Shadow;
					}
					if (node->Kind == core::Name("last-frame")) {
						return ResourceRole::PreviousFrame;
					}
					if (node->Kind == core::Name("surface-capture")) {
						constexpr std::array roles{
							ResourceRole::Surface, ResourceRole::PortalImage, ResourceRole::PortalLight
						};
						return output < roles.size() ? roles[output] : ResourceRole::Unknown;
					}
					if (node->Kind == core::Name("mirror-capture")) {
						return ResourceRole::Surface;
					}
					if (node->Kind == core::Name("portal-capture")) {
						// Output 0 is the recursion pool, output 1 the seam
						// light-field atlas - the default document's order.
						return output == 0 ? ResourceRole::PortalImage : ResourceRole::PortalLight;
					}
					if (node->Kind == core::Name("portal-tonemap")) {
						return ResourceRole::PortalDisplay;
					}
					if (node->Kind == core::Name("gbuffer")) {
						constexpr std::array roles{
							ResourceRole::Albedo,
							ResourceRole::Normal,
							ResourceRole::Material,
							ResourceRole::Emissive,
							ResourceRole::Depth,
						};
						return output < roles.size() ? roles[output] : ResourceRole::Unknown;
					}
					if (node->Kind == core::Name("forward")) {
						return output == 1 ? ResourceRole::Depth : ResourceRole::Unknown;
					}
					if (node->Kind == core::Name("depth-linearise")) {
						const auto *background = node->Parameter(core::Name("background"));
						// Exported zero-background depth must not overwrite lighting's far-background
						// texture.
						return background && *background == "zero" ? ResourceRole::Unknown
																   : ResourceRole::LinearDepth;
					}
					if (node->Kind == core::Name("ssao")) {
						return ResourceRole::Occlusion;
					}
					if (node->Kind == core::Name("deferred-lighting")) {
						return output < node->WritePorts.size() &&
									   (node->WritePorts[output] == core::Name("lighting-baseline") ||
										node->WritePorts[output] == core::Name("directional-response"))
								   ? ResourceRole::Unknown
								   : ResourceRole::Lit;
					}
					if (node->Kind == core::Name("sky")) {
						return ResourceRole::SkyLit;
					}
					if (node->Kind == core::Name("fog")) {
						return ResourceRole::VolumeLit;
					}
				}
			}
			return ResourceRole::Unknown;
		}

		struct PreviewReadback {
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t Capacity = 0;
			SDL_GPUFence *Fence = nullptr;
			core::Name Source;
			size_t Slot = Renderer::ANY_VIEWPORT;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t BytesPerPixel = 0;
			bool Rgba = false;
			std::vector<uint32_t> Pixels;
			ImageHistogram Histogram;
			PendingReadback Pending;
		};

		PreviewReadback Preview;
		core::Name Inspected;
		size_t InspectedSlot = Renderer::ANY_VIEWPORT;
		uint64_t FrameCounter = 0;
		bool PreviewSubmitted = false;

		// A multi-view frame lends one command buffer to each view in turn. The
		// ordinary Render body still records a complete view, while these fields
		// keep acquisition, graph scope, timings, and submission frame-owned.
		bool BatchActive = false;
		bool BatchFirst = false;
		bool BatchFinal = false;
		bool BatchShared = false;
		// All worlds share mesh buffers, so the residency node records at most
		// one delta for one submitted frame.
		bool MeshResidencyRecorded = false;
		FramePreparation PreparedScopes;
		bool BatchFailed = false;
		size_t BatchViewIndex = 0;
		size_t BatchWorldIndex = 0;
		SDL_GPUCommandBuffer *BatchCommand = nullptr;
		SDL_GPUTexture *BatchSwapchain = nullptr;
		uint32_t BatchWidth = 0;
		uint32_t BatchHeight = 0;
		uint32_t BatchTimingSlot = VulkanTimestamps::NO_SLOT;

		// Later-transfer command buffer for previews and file captures. Owned HDR
		// exports copy at their declared graph read in the main buffer, before
		// resource alias reuse; those share the scene submission fence instead.
		// SDL's one unified queue executes submissions in order, so the copies
		// read the frame's finished images without a fence between the two.
		// Physical overlap is not available on that queue; the split is the
		// structural boundary `graph::PlanCommandBuffers` plans, in place for a
		// backend with an independent transfer queue.
		SDL_GPUCommandBuffer *DownloadCommand = nullptr;

		SDL_GPUCommandBuffer *DownloadBuffer() {
			if (DownloadCommand == nullptr) {
				DownloadCommand = SDL_AcquireGPUCommandBuffer(Device);
				if (DownloadCommand == nullptr) {
					ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (downloads): {}", SDL_GetError());
				}
			}
			return DownloadCommand;
		}

		// A frame that failed after downloads were recorded: the buffer is
		// cancelled rather than submitted, and a pending preview stops waiting
		// for pixels that will never arrive.
		void DropDownloads() {
			if (DownloadCommand != nullptr) {
				SDL_CancelGPUCommandBuffer(DownloadCommand);
				DownloadCommand = nullptr;
			}
			if (PreviewSubmitted) {
				Preview.Pending.Poll(true);
				Preview.Pixels.clear();
				Preview.Histogram = ImageHistogram{};
				PreviewSubmitted = false;
			}
		}

		void CollectPreview();
		void PollPreview();
		bool RequestPreview(
			SDL_GPUCommandBuffer *command,
			SDL_GPUTexture *texture,
			uint32_t width,
			uint32_t height,
			core::Name source,
			size_t slot,
			SDL_GPUTextureFormat format
		);

		struct PassMarks {
			core::Name Name;
			uint32_t Opened = VulkanTimestamps::MARKS;
			uint32_t Closed = VulkanTimestamps::MARKS;
		};

		VulkanTimestamps Timestamps;
		std::array<std::vector<PassMarks>, VulkanTimestamps::SLOTS> PendingMarks;
		std::array<uint64_t, VulkanTimestamps::SLOTS> TimingSequence{};
		uint64_t NextTimingSequence = 1;
		uint64_t ResolvedTimingSequence = 0;
		std::unordered_map<uint32_t, double> GpuTimings;
		std::unordered_map<uint32_t, double> WallTimings;
		ProfilingTier ProfileTier = ProfilingTier::Full;
		uint32_t ProfileSampleRate = 4;
		size_t DroppedProfileMarks = 0;

		// The frame-graph span name for each pass's *device* time, keyed by the
		// pass's `core::Name::Id`.
		//
		// **Decorated, because the undecorated name is already taken.**
		// `GraphRunner` opens a CPU scope named after the node - `ssao`,
		// `gbuffer` - so reporting device time under the same name puts two spans
		// with one label in every frame, and the profiler's per-span table sums
		// them. That is precisely the CPU-versus-GPU confusion
		// `ProfileCategory::Gpu` was added to end, arriving through the label
		// instead of through the colour.
		//
		// **Cached rather than built per frame**, and not for the allocation:
		// `FrameGraph::Report` borrows the text and the overlay reads it after
		// the frame has ended, so the string has to outlive the frame. Owning it
		// here is what makes that true, and a pass name is registered once and
		// then read every frame for the life of the process.
		std::unordered_map<uint32_t, std::string> GpuSpanNames;

		// The span name for one pass's device time, registering it on first use.
		std::string_view GpuSpanName(const core::Name &pass) {
			auto found = GpuSpanNames.find(pass.Id());
			if (found == GpuSpanNames.end()) {
				found = GpuSpanNames.emplace(pass.Id(), "gpu " + std::string(pass.Text())).first;
			}
			return found->second;
		}

		void CollectTimings();

		SDL_GPUGraphicsPipeline *OpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrWireframeOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrWireframeTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *ForwardPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedHdrOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedHdrTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedForwardPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedGBufferPipeline = nullptr;

		// The two above, redrawn as lines. See where they are created for why
		// there are two objects and not a bindable state.
		SDL_GPUGraphicsPipeline *WireframeOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *WireframeTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedWireframeOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedWireframeTransparentPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedHdrWireframeOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedHdrWireframeTransparentPipeline = nullptr;

		// Whether `BindPipeline` should hand out the pair above instead of the
		// ordinary two. Off unless a caller has asked - `Renderer::
		// SetWireframe` is the only door.
		bool WireframeMode = false;

		// Whether every instance draws with the default texture rather than its
		// own. `Renderer::SetUntextured` is the only door; see `DrawSlots`,
		// where the substitution happens.
		bool UntexturedMode = false;

		// The default graph's opaque path. Geometry writes material properties,
		// then screen-sized passes consume those textures. Portal panes and the
		// blended tail stay on the forward family below because their projected
		// images and ordering are not representable by one G-buffer pixel.
		SDL_GPUGraphicsPipeline *GBufferPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DepthLinearPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DepthComposePipeline = nullptr;
		bool EnsureDepthCompose();
		SDL_GPUGraphicsPipeline *ColourComposePipeline = nullptr;
		bool EnsureColourCompose();
		SDL_GPUGraphicsPipeline *TransparentLayerPipeline = nullptr;
		SDL_GPUGraphicsPipeline *TransparentLayerColourPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedTransparentLayerPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedTransparentLayerColourPipeline = nullptr;
		bool EnsureTransparentLayer();
		SDL_GPUGraphicsPipeline *InterfaceLayerPipeline = nullptr;
		SDL_GPUGraphicsPipeline *InterfaceLayerColourPipeline = nullptr;
		bool EnsureInterfaceLayer();
		SDL_GPUGraphicsPipeline *SsaoPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DeferredLightingPipeline = nullptr;
		SDL_GPUGraphicsPipeline *SkyPipeline = nullptr;
		SDL_GPUGraphicsPipeline *VolumePipeline = nullptr;
		SDL_GPUGraphicsPipeline *TonemapPipeline = nullptr;
		SDL_GPUComputePipeline *EnvironmentSkyCompute = nullptr;
		SDL_GPUComputePipeline *EnvironmentCloudCompute = nullptr;

		// Each owner selects one grade. Missing owners use the engine tonemap.
		struct PostProcessVariant {
			core::Name Shader;
			assets::ContentHash CodeHash;
			SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		};
		std::unordered_map<uint32_t, PostProcessVariant> PostProcessPipelines;

		struct PbrDimensions {
			uint32_t TargetWidth = 0;
			uint32_t TargetHeight = 0;
			uint32_t ViewWidth = 0;
			uint32_t ViewHeight = 0;
			uint32_t LinearWidth = 0;
			uint32_t LinearHeight = 0;
			uint32_t OcclusionWidth = 0;
			uint32_t OcclusionHeight = 0;
			uint32_t LitWidth = 0;
			uint32_t LitHeight = 0;

			bool operator==(const PbrDimensions &) const = default;
		};

		struct PbrSlot {
			SDL_GPUTexture *Albedo = nullptr;
			SDL_GPUTexture *Normal = nullptr;
			SDL_GPUTexture *Material = nullptr;
			SDL_GPUTexture *Emissive = nullptr;
			SDL_GPUTexture *LinearDepth = nullptr;
			SDL_GPUTexture *Occlusion = nullptr;
			SDL_GPUTexture *Lit = nullptr;
			SDL_GPUTexture *SkyLit = nullptr;
			PbrDimensions Dimensions;
		};

		std::vector<PbrSlot> PbrSlots;

		PbrSlot &PbrAt(size_t slot) {
			if (PbrSlots.size() <= slot) {
				PbrSlots.resize(slot + 1);
			}
			return PbrSlots[slot];
		}

		bool EnsurePbr(size_t slot, const PbrDimensions &dimensions);
		void ReleasePbr(PbrSlot &slot);

		// Graph-owned history images hold each world's sky and cloud layers. This
		// cache only records their completed content signatures, so regeneration
		// is skipped until an authored provider or resident source handle changes.
		struct EnvironmentTarget {
			SDL_GPUTexture *SkyTarget = nullptr;
			SDL_GPUTexture *CloudTarget = nullptr;
			uint64_t SkySignature = 0;
			uint64_t CloudSignature = 0;
			bool SkyReady = false;
			bool CloudReady = false;
		};

		std::vector<EnvironmentTarget> Environments;
		bool RecordEnvironmentSkybox(
			const scene::Environment &environment,
			SDL_GPUCommandBuffer *command,
			SDL_GPUTexture *destination,
			uint32_t width,
			uint32_t height,
			uint32_t &dispatches
		);
		bool RecordEnvironmentClouds(
			const scene::Environment &environment,
			SDL_GPUCommandBuffer *command,
			SDL_GPUTexture *source,
			SDL_GPUTexture *destination,
			uint32_t width,
			uint32_t height,
			uint32_t &dispatches
		);
		void ReleaseEnvironments();

		struct NamedTexture {
			SDL_GPUTexture *Texture = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;

			bool IsValid() const {
				return Texture != nullptr && Width > 0 && Height > 0;
			}
		};

		// Graph targets are isolated by the pipeline that declared them and by
		// the scope that produced them. A view target belongs to a viewport slot,
		// while world work belongs to the caller's stable world key. This is the
		// storage rule that prevents two worlds with identically named resources
		// from sampling one another.
		struct GraphTarget {
			core::Name Pipeline;
			core::Name Resource;
			graph::NodeScope Scope = graph::NodeScope::Frame;
			uint64_t Owner = 0;
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint64_t HistorySignature = 0;
			bool HistoryReady = false;
		};

		std::vector<GraphTarget> GraphTargets;

		// A history image becomes readable only after the command buffer that wrote
		// it has entered the queue. Batched views share one command buffer, so this
		// state belongs to the renderer rather than one view recording.
		struct PendingGraphHistoryWrite {
			const NamedPipeline *Pipeline = nullptr;
			core::Name Resource;
			graph::NodeScope Scope = graph::NodeScope::Frame;
			uint64_t Owner = 0;
			uint64_t Signature = 0;
		};

		std::vector<PendingGraphHistoryWrite> PendingGraphHistoryWrites;

		// Buffer resources use the same ownership key as images. A graph can name
		// the same buffer in several views without letting one view overwrite another.
		struct GraphBuffer {
			core::Name Pipeline;
			core::Name Resource;
			graph::NodeScope Scope = graph::NodeScope::Frame;
			uint64_t Owner = 0;
			SDL_GPUBuffer *Buffer = nullptr;
			uint32_t Bytes = 0;
			SDL_GPUBufferUsageFlags Usage = 0;
		};

		// Authored buffers are transient scratch. These caps bound a document before
		// one unchecked width, height and stride turn into an unbounded allocation.
		static constexpr uint64_t MAX_GRAPH_BUFFER_BYTES = 64u * 1024u * 1024u;
		static constexpr uint64_t MAX_GRAPH_BUFFER_TOTAL_BYTES = 256u * 1024u * 1024u;
		std::vector<GraphBuffer> GraphBuffers;

		struct ResourcePreviewTarget {
			ResourcePreviewRoute Route;
			std::array<SDL_GPUTexture *, 2> Textures{};
			ResourcePreviewSlots Slots;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool ReverseSpectrum = false;
			bool Refresh = true;
		};

		std::vector<ResourcePreviewTarget> ResourcePreviews;

		graph::NodeScope ResourceScope(const NamedPipeline &pipeline, graph::ResourceId resource) const;
		NamedTexture FindGraphTarget(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner
		) const;
		NamedTexture FindGraphHistoryForRead(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner,
			uint64_t signature
		) const;
		void CommitGraphHistoryWrite(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner,
			uint64_t signature
		);
		void StageGraphHistoryWrite(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner,
			uint64_t signature
		);
		void CommitPendingGraphHistoryWrites();
		void DiscardPendingGraphHistoryWrites();
		core::Name GraphTargetName(const NamedPipeline &pipeline, core::Name resource) const;
		NamedTexture EnsureGraphTarget(
			const NamedPipeline &pipeline,
			graph::ResourceId resource,
			uint64_t owner,
			uint32_t viewWidth,
			uint32_t viewHeight
		);
		SDL_GPUBuffer *FindGraphBuffer(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner
		) const;
		SDL_GPUBuffer *EnsureGraphBuffer(
			const NamedPipeline &pipeline,
			graph::ResourceId resource,
			uint64_t owner,
			uint32_t viewWidth,
			uint32_t viewHeight
		);

		struct GraphRasterPipeline {
			core::Name Pipeline;
			core::Name Node;
			std::vector<SDL_GPUTextureFormat> Formats;
			uint32_t Samplers = 0;
			SDL_GPUGraphicsPipeline *Handle = nullptr;
		};

		struct GraphComputePipeline {
			core::Name Pipeline;
			core::Name Node;
			uint32_t Samplers = 0;
			uint32_t Storage = 0;
			uint32_t ReadStorage = 0;
			uint32_t Uniforms = 0;
			uint32_t LocalX = 1;
			uint32_t LocalY = 1;
			uint32_t LocalZ = 1;
			SDL_GPUComputePipeline *Handle = nullptr;
		};

		std::vector<GraphRasterPipeline> GraphRasterPipelines;
		std::vector<GraphComputePipeline> GraphComputePipelines;
		struct GraphCaptureReceipt {
			core::Name Pipeline;
			core::Name Node;
			std::string Path;
		};
		std::vector<GraphCaptureReceipt> GraphCaptureReceipts;
		std::unordered_map<std::string, uint64_t> GraphCaptureFrames;

		bool GraphShaderCode(
			const graph::Node &node, ShaderStage stage, std::vector<uint8_t> &bytes, std::string &entryPoint
		) const;
		SDL_GPUGraphicsPipeline *GraphRasterFor(
			const NamedPipeline &pipeline,
			const graph::Node &node,
			std::span<const SDL_GPUTextureFormat> formats,
			uint32_t samplers
		);
		SDL_GPUComputePipeline *GraphComputeFor(
			const NamedPipeline &pipeline,
			const graph::Node &node,
			uint32_t samplers,
			uint32_t storage,
			uint32_t readStorage,
			uint32_t uniforms,
			uint32_t localX,
			uint32_t localY,
			uint32_t localZ
		);
		void ReleaseGraphState(core::Name pipeline);
		void ReleaseAllGraphState();

		// The same geometry and the same shaders as the opaque pipeline, with
		// blending on and depth writes off. Two pipelines rather than one with
		// a uniform, because blend state is baked into a pipeline on every
		// modern API and cannot be changed by a draw call.
		SDL_GPUGraphicsPipeline *TransparentPipeline = nullptr;

		// The ground grid, drawn in the transparent pass. See `grid.frag`: a
		// fullscreen triangle that finds the ground plane per pixel and writes
		// its own `gl_FragDepth`, so the hardware depth test is what hides it
		// behind a wall. Null when the shader failed to build, which disables
		// the grid and nothing else.
		SDL_GPUGraphicsPipeline *GridPipeline = nullptr;

		// --- shader variants --------------------------------------------------
		//
		// **A pipeline per named shader, per family, and no more general than
		// that.** `scene::MaterialRef::Shader` selects a *fragment* shader -
		// `scene::ShaderSource` says why only that stage - so a variant is the
		// opaque and transparent pipelines with one shader object swapped and
		// everything else identical. The vertex layout, the depth state and the
		// blend state are the renderer's and stay the renderer's.
		//
		// Blend state and attachment format are baked into each pipeline, so
		// opaque and transparent draws each have display and HDR variants.
		//
		// **The shadow pass has no variant and must not grow one.** It writes
		// depth and no colour, so a fragment shader that computed a colour would
		// cost a pass over the whole scene to produce nothing.
		//
		// This is a table of *substitutions*, not the beginning of a render
		// graph. When the node system arrives it is what says which shader a
		// pass wants, and this table becomes the backend that answers - so do
		// not grow a second way to describe a frame here in the meantime.
		struct ShaderVariant {
			assets::ContentHash CodeHash;
			SDL_GPUShader *Fragment = nullptr;
			SDL_GPUGraphicsPipeline *Opaque = nullptr;
			SDL_GPUGraphicsPipeline *Transparent = nullptr;
			SDL_GPUGraphicsPipeline *HdrOpaque = nullptr;
			SDL_GPUGraphicsPipeline *HdrTransparent = nullptr;
			SDL_GPUGraphicsPipeline *PackedOpaque = nullptr;
			SDL_GPUGraphicsPipeline *PackedTransparent = nullptr;
			SDL_GPUGraphicsPipeline *PackedHdrOpaque = nullptr;
			SDL_GPUGraphicsPipeline *PackedHdrTransparent = nullptr;
		};

		// A material name resolves within its residency owner.
		static uint64_t ShaderVariantKey(core::Name name, core::Name owner) {
			return (uint64_t(owner.Id()) << 32) | name.Id();
		}
		std::unordered_map<uint64_t, ShaderVariant> ShaderVariants;

		// Lens pipelines are separate from material variants because their fixed
		// fragment interface samples scene images rather than material textures.
		struct LensVariant {
			assets::ContentHash CodeHash;
			SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		};
		std::unordered_map<uint64_t, LensVariant> LensPipelines;
		struct PortalLensProgramBinding {
			core::Name Shader;
			assets::ContentHash Hash;
			SDL_GPUGraphicsPipeline *Pipeline = nullptr;
			bool operator==(const PortalLensProgramBinding &other) const {
				return Shader == other.Shader && Hash == other.Hash;
			}
		};
		struct PortalLensProgramGroup {
			uint64_t Token = 0;
			size_t References = 0;
			std::vector<PortalLensProgramBinding> Bindings;
		};
		// Captured programs have independent residency from authored world namespaces.
		std::array<PortalLensProgramGroup, 256> PortalLensPrograms;
		SDL_GPUGraphicsPipeline *CreateLensPipeline(core::Name name, std::span<const uint32_t> spirv);

		// Failed material, lens and grade attempts are bounded by current owner demand.
		std::array<std::unordered_map<uint64_t, assets::ContentHash>, 3> ShaderRefusals;

		// The opaque vertex shader, kept rather than released.
		//
		// **Every other shader object is released once its pipeline holds it**,
		// and this one cannot be: a variant is built when a shader arrives,
		// which is any time after `CreatePipelines` ran, and it needs the same
		// vertex stage the opaque pipeline was built with. Building a second one
		// from the file would be two objects for one shader, free to disagree
		// the day `opaque.vert` changes shape.
		SDL_GPUShader *OpaqueVertexShader = nullptr;
		SDL_GPUShader *PackedOpaqueVertexShader = nullptr;

		// The two descriptors a variant is derived from, kept whole.
		//
		// **Members rather than the locals `CreatePipelines` builds**, because a
		// `SDL_GPUGraphicsPipelineCreateInfo` is a struct of *pointers* into
		// arrays the caller owns - so keeping the info and letting the arrays go
		// out of scope is a dangling read at the next `AddShader`. The arrays
		// live here and the infos point at them.
		//@{
		SDL_GPUVertexBufferDescription VariantBuffers[1]{};
		SDL_GPUVertexAttribute VariantAttributes[5]{};
		SDL_GPUColorTargetDescription VariantOpaqueTarget{};
		SDL_GPUColorTargetDescription VariantBlendedTarget{};
		SDL_GPUGraphicsPipelineCreateInfo VariantOpaqueInfo{};
		SDL_GPUGraphicsPipelineCreateInfo VariantBlendedInfo{};
		bool VariantsReady = false;
		//@}

		// Which family the open pass last bound, so `DrawSlots` knows which
		// variant a slot's shader means and what to put back afterwards.
		enum class PipelineFamily : uint8_t {
			// Anything with no variants: the shadow, overlay, particle and
			// ribbon passes. A slot's shader is ignored while one is bound.
			Other,
			GBuffer,
			Opaque,
			Transparent,
			HdrOpaque,
			HdrTransparent,
		};

		PipelineFamily ActiveFamily = PipelineFamily::Other;
		SDL_GPUGraphicsPipeline *ActivePipeline = nullptr;

		// The submission order, rebuilt each frame and kept so it is not
		// reallocated per frame. See `scene::OrderForDrawing`.
		std::vector<uint32_t> DrawOrder;

		// Entity-list and viewpoint storage belongs to the renderer rather than one
		// `RenderView` call. Filter chains clear their contents between views while
		// retaining the capacity paid for by the largest view seen so far.
		graph::EntityFlow GraphEntities;
		graph::Viewpoints GraphViewpoints;

		// What survived culling: the indices, and the instances themselves.
		//
		// The copy makes scene and camera ranges contiguous in one buffer.
		std::vector<uint32_t> Visible;
		std::vector<scene::DrawInstance> VisibleInstances;

		// The whole draw list, ordered for the surface camera. What the shadow
		// pass and the surface pass draw, because neither is the eye's: a caster
		// off screen still shadows, and a mirror shows what is behind the
		// viewer.
		std::vector<scene::DrawInstance> SceneInstances;
		std::vector<core::CFrame> SceneJointFrames;

		// The instances whose geometry has arrived, which is what every pass
		// below works from. See the filter in `Render` for why an instance
		// naming an absent mesh is dropped rather than drawn as a cube.
		//
		// Kept on the state rather than made per frame, so a steady scene stops
		// allocating after its first one - the rule every buffer here follows.
		std::vector<scene::DrawInstance> Drawable;
		std::vector<uint32_t> DrawableHidden;

		// The same, for the rows belonging to *other* worlds - see `Render`'s
		// `foreign` argument. A separate buffer rather than a tail on `Drawable`
		// because every pass but the surface pass must not see these, and a
		// shared buffer is one `.size()` away from them all seeing them.
		std::vector<scene::DrawInstance> DrawableForeign;

		std::vector<uint32_t> SceneOrder;

		// Inverse of `SceneOrder`, from an own-world source row to its packed
		// scene slot. The camera range contains a subset of those exact rows, so
		// it can reuse their resident slots instead of resolving and packing each
		// visible instance a second time.
		std::vector<uint32_t> SceneSlotOfSource;
		// The target whose own-world metadata still occupies the shared slot
		// arrays. A repeated single-target view can retain that dense prefix.
		size_t PackedMetadataTarget = std::numeric_limits<size_t>::max();
		SDL_GPUGraphicsPipeline *ImagePipeline = nullptr;
		SDL_GPUGraphicsPipeline *OverlayPipeline = nullptr;

		// Every mesh and texture available to the renderer.
		MeshTable Meshes;
		TextureTable Textures;
		// Named resources can change without changing any submitted draw row.
		// Active view scope for native environment, particle and ribbon texture bindings.
		core::Name ActiveContentOwner;
		static core::Name MeshContentOwner(core::Name name, core::Name owner);
		static core::Name TextureContentOwner(core::Name name, core::Name owner);

		uint64_t ResourceEpoch = 0;

		// How long animation has been running, as the caller measures it.
		//
		// **Given rather than read.** `render` holds no clock, which is the
		// standing rule `assets::Grant` and `cdn::Service::Pump` keep for the
		// same reason: a module with a notion of "now" of its own has one to
		// drift, and a recorded run could not replay.
		double AnimationSeconds = 0.0;

		// How many levels of mirror-in-mirror to resolve, or zero to measure it.
		//
		// **Zero by default, which is `scene::AUTOMATIC_SURFACE_BOUNCES`.** This
		// was welded at two for two versions and every scene resolved exactly
		// two levels whatever it was built from - a room with one mirror paying
		// for a level that could never show anything, and a corridor of facing
		// panes cut off one level into the effect. No constant is right for
		// both, and since the levels became a recursion the cost of guessing
		// high multiplies rather than adds: `panes × (panes - 1) ^ (levels - 1)`
		// where the old iteration went as `panes × levels`.
		//
		// So the ordinary case is that nobody states one and each viewport's
		// bank measures what the frame it just drew actually reached - see
		// `SurfaceBank::Bounces` and `scene::NextSurfaceBounces`. A world that
		// does state one (`workspace.SurfaceBounces`) or a run that does
		// (`--surface-bounces`) overrides the measurement outright, because an
		// author who has typed a number is not asking for a negotiation.
		uint32_t SurfaceBounces = 0;

		// How many surface panes a viewport may draw in one frame.
		//
		// **Held rather than taken per frame, for `SurfaceBounces`' reason**: it
		// is a property of the world being drawn rather than of this frame, and
		// a host writes it from `scene::SurfaceLimitOf` before each world's
		// render. See `Renderer::SetSurfaceLimit` for why the budget is spent on
		// the panes covering the most screen rather than on the first ones in
		// the list.
		uint32_t SurfaceLimit = static_cast<uint32_t>(scene::DEFAULT_SURFACE_LIMIT);

		// How many levels the recursive portal pass goes to.
		//
		// **Two, which is a hole seen through a hole resolved inside the frame
		// from the right viewpoint at each level.** That is what the recursive
		// pass exists for; one level draws the nested pane flat.
		//
		// **This used to be one, and the comment that held it there described a
		// device hang that no longer happens.** It reported twenty of twenty
		// runs of `Portals-1-world.luau` hanging in `SDL_WaitForGPUIdle` at
		// shutdown at depth two against none of thirteen at depth one, and
		// blamed a write-after-read inside one command buffer: a level-zero
		// target rendered into once per level-one hole, then sampled, then
		// written again. It named the fix it was waiting for as a command
		// buffer per top-level hole, so that each level-zero target would be
		// written once and read once.
		//
		// **It got that property another way, and the comment was never
		// revisited.** `PortalTarget` carries a `Display` texture beside its
		// `Colour` one now: `portal-tonemap` copies the finished level into
		// `Display` and `portal-overlay` samples `Display`, so the target a
		// level writes is not the texture a pane reads and the chain the hang
		// was pinned to is gone.
		//
		// Re-measured at v0.19 on the same scene at the same twenty frames,
		// `release` preset: twenty of twenty headless runs and five of five
		// windowed runs exit zero at depth two. The default is what the shipped
		// engine draws with, because **nothing calls `SetPortalDepth`** - the
		// knob is public and unclamped below `MAX_PORTAL_DEPTH`, and no host has
		// ever turned it. Depths three and four are therefore unreachable and
		// unmeasured; raising this past two wants its own run of the same test.
		uint32_t PortalDepth = 2;

		// The world's directional light, as the shader wants it.
		//
		// **Held rather than taken per frame, for `SetSurfaceBounces`' reason**:
		// it is a property of what is being drawn rather than of this frame, and
		// threading it through `Render` would put it in a signature every host
		// calls to say the same thing on every frame. `client::Client::Frame`
		// writes it from `scene::LightingOf` before each world's render.
		glm::vec3 Sun{SUN_DIRECTION};
		glm::vec4 Ambient{SUN_AMBIENT};
		glm::vec4 OutdoorAmbient{0.0f, 0.0f, 0.0f, 1.0f};
		glm::vec4 Direct{1.0f, 1.0f, 1.0f, 1.0f};
		glm::vec4 FogColour{0.05f, 0.06f, 0.09f, 1.0f};
		float FogStart = 100000.0f;
		float FogEnd = 100001.0f;
		scene::Environment EnvironmentState;
		std::array<scene::VolumeState, scene::MAX_SCENE_VOLUMES> Volumes{};
		size_t VolumeCount = 0;
		std::array<scene::ShaderLensState, scene::MAX_SCENE_SHADER_LENSES> ShaderLenses{};
		size_t ShaderLensCount = 0;

		// What is in each slot of the instance buffer, filled in the same loop
		// that fills the buffer itself.
		//
		// **Parallel arrays rather than a struct**, because the draw loop
		// compares consecutive entries to find its runs and does it far more
		// often than it reads them.
		std::vector<const MeshEntry *> SlotMesh;
		std::vector<core::Name> SlotTexture;
		std::vector<core::Name> SlotContentOwner;
		std::vector<core::Name> SlotNormalMap;
		std::vector<core::Name> SlotRoughnessMap;
		std::vector<core::Name> SlotOcclusionMap;
		std::vector<core::Name> SlotHeightMap;
		std::vector<core::Name> SlotMetalnessMap;
		std::vector<core::Name> SlotEmissiveMap;
		std::vector<scene::SurfaceResampleMode> SlotResample;
		// Whether a shadow run needs per-material alpha or seam state.
		std::vector<uint8_t> SlotShadowDetail;

		// Which shader each slot asks for, or an invalid name for the engine's.
		//
		// **A name per slot rather than a resolved pipeline**, because the run
		// loop compares consecutive entries far more often than it uses one -
		// the same reason every array here is parallel rather than a struct. The
		// lookup happens once per run, where the pipeline is bound.
		std::vector<core::Name> SlotShader;

		// Each slot's tag mask, for the surface passes that filter by one.
		std::vector<uint32_t> SlotTags;

		// The half-space each slot keeps, as a world plane: xyz the unit normal,
		// w the offset, and a zero normal for "whole".
		//
		// **A per-slot plane rather than a run of its own in the plan.** A body
		// standing in a portal is cut at the pane and its far half is drawn in the
		// room beyond; the cut is per instance and every uniform here is per frame
		// or per draw, so it is carried the way the tag mask is - the run breaks
		// where the plane changes, exactly as it breaks where the mesh does. A
		// world with no hole in it holds one value throughout and pays one compare
		// per instance for it. See `scene::DrawInstance::SeamNormal`.
		std::vector<glm::vec4> SlotSeam;

		// Which way the sun comes from for each slot, or a zero vector for the
		// world's own. `scene::DrawInstance::SeamLight` carries the argument.
		std::vector<glm::vec4> SlotSeamLight;
		std::vector<InstanceKey> SlotInstanceKey;
		std::vector<uint8_t> SlotInstanceCurrent;
		std::vector<uint32_t> SlotLod;

		SDL_GPUBuffer *InstanceBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceTransfer = nullptr;
		uint32_t InstanceCapacity = 0;
		SDL_GPUBuffer *InstanceIndexBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceIndexTransfer = nullptr;
		uint32_t InstanceIndexCapacity = 0;
		SDL_GPUBuffer *SkinOffsetBuffer = nullptr;
		SDL_GPUTransferBuffer *SkinOffsetTransfer = nullptr;
		SDL_GPUBuffer *JointBuffer = nullptr;
		SDL_GPUTransferBuffer *JointTransfer = nullptr;

		// Packed rows are world-owned. Several cameras looking at one world share
		// this pool and upload changed rows once; each target keeps only its own
		// visibility and draw-order index stream.
		struct InstanceWorld {
			uint64_t Id = 0;
			core::Name Name;
			InstanceResidency Instances;
			SDL_GPUBuffer *Buffer = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t Capacity = 0;
			uint64_t MetricFrame = 0;
		};
		std::vector<InstanceWorld> InstanceWorlds;
		InstanceWorld *ActiveInstanceWorld = nullptr;
		struct PendingInstanceUpload {
			uint64_t Id = 0;
			core::Name Name;
		};
		std::vector<PendingInstanceUpload> PendingInstanceUploads;

		// --- authored mesh LOD -----------------------------------------------
		//
		// One compact set per view. Host code packs each level against its own
		// mesh bounds. The compute node chooses the level by projected pixel area
		// and enables only that level's indexed indirect commands.
		struct LodState {
			SDL_GPUComputePipeline *Select = nullptr;
			SDL_GPUBuffer *Selections = nullptr;
			SDL_GPUBuffer *Clusters = nullptr;
			SDL_GPUBuffer *Instances = nullptr;
			SDL_GPUBuffer *Indices = nullptr;
			SDL_GPUBuffer *SkinOffsets = nullptr;
			SDL_GPUBuffer *Arguments = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t SelectionCapacity = 0;
			uint32_t ClusterCapacity = 0;
			uint32_t InstanceCapacity = 0;
			uint32_t ArgumentCapacity = 0;
			uint32_t TransferCapacity = 0;
			bool Ready = false;
		};
		LodState Lod;
		LodPlan LodFrame;

		bool EnsureLodResources(uint32_t selections, uint32_t instances, uint32_t arguments);
		bool DispatchLodSelection(
			SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection, uint32_t width, uint32_t height
		);
		void ReleaseLod();

		// --- occlusion culling ------------------------------------------------
		//
		// The machinery behind `culling = "occlusion"` on an authored entity
		// filter node: a depth pyramid seeded by the occluders the CPU picked,
		// a compute pass that tests every remaining opaque instance's box
		// against it, and indexed indirect draw arguments so the survivor
		// counts never make the round trip back to the CPU.
		//
		// **The pyramid is separate textures rather than one texture's mips.**
		// A compute pass cannot bind one mip of a texture for writing while
		// sampling another mip of the same texture, and SDL's read-only
		// storage-texture binding cannot name a mip at all. Twelve levels cover
		// a screen rectangle up to 2048 texels wide; a candidate wider than
		// that is declared visible, which is the conservative direction.
		static constexpr uint32_t PYRAMID_LEVEL_LIMIT = 12;

		struct OcclusionState {
			SDL_GPUComputePipeline *Seed = nullptr;
			SDL_GPUComputePipeline *Reduce = nullptr;
			SDL_GPUComputePipeline *Cull = nullptr;
			SDL_GPUComputePipeline *Args = nullptr;

			SDL_GPUTexture *Levels[PYRAMID_LEVEL_LIMIT] = {};
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t LevelCount = 0;

			// The per-frame buffers, all sized in whole entries and grown in
			// powers of two like `InstanceBuffer`. One transfer buffer stages
			// everything the CPU writes, packed back to back.
			SDL_GPUBuffer *Arguments = nullptr;	 // indexed indirect commands, early then late
			SDL_GPUBuffer *Candidates = nullptr; // two vec4 per candidate
			SDL_GPUBuffer *RunTable = nullptr;	 // per slot-run: first late slot
			SDL_GPUBuffer *ArgRuns = nullptr;	 // per late argument: its slot-run
			SDL_GPUBuffer *Counts = nullptr;	 // per slot-run: survivor count
			SDL_GPUBuffer *LateIndices = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t ArgumentCapacity = 0;
			uint32_t CandidateCapacity = 0;
			uint32_t RunCapacity = 0;
			uint32_t LateCapacity = 0;
			uint32_t TransferCapacity = 0;
		};
		OcclusionState Occlusion;

		// What this frame's cull draws, decided while the instances convert.
		//
		// `RunEarly[r]` instances at the head of slot-run `r` are the occluders
		// the CPU picked; the `RunCandidates[r]` behind them wait on the GPU
		// test. Both phases walk the same runs `DrawSlots` walks, so the
		// argument order is the walk order and nothing stores a mapping.
		struct OcclusionPlan {
			bool Active = false;
			uint32_t RunCount = 0;
			uint32_t ArgCount = 0; // per phase: one per slot-run material range
			uint32_t CandidateCount = 0;
			uint32_t EarlyTotal = 0;
			std::vector<uint32_t> RunEarly;
			std::vector<uint32_t> RunCandidates;
			std::vector<uint32_t> RunFirstSlot;
			// Two vec4 per candidate, already in the layout the cull reads -
			// see occlusion-cull.comp.
			std::vector<glm::vec4> CandidatePairs;
		};
		OcclusionPlan OcclusionFrame;

		// Whether two slots may share one instanced draw. Everything a run
		// binds per draw has to agree, so this is the run-break rule - the one
		// `DrawSlots` walks with and the occlusion plan must walk with, or the
		// plan's argument order names the wrong runs.
		bool SlotsShareRun(uint32_t slot, uint32_t next) const {
			return SlotLod[next] == SlotLod[slot] && SlotMesh[next] == SlotMesh[slot] &&
				   SlotTexture[next] == SlotTexture[slot] &&
				   SlotContentOwner[next] == SlotContentOwner[slot] &&
				   SlotNormalMap[next] == SlotNormalMap[slot] &&
				   SlotRoughnessMap[next] == SlotRoughnessMap[slot] &&
				   SlotOcclusionMap[next] == SlotOcclusionMap[slot] &&
				   SlotHeightMap[next] == SlotHeightMap[slot] &&
				   SlotMetalnessMap[next] == SlotMetalnessMap[slot] &&
				   SlotEmissiveMap[next] == SlotEmissiveMap[slot] &&
				   SlotResample[next] == SlotResample[slot] && SlotShader[next] == SlotShader[slot] &&
				   SlotSeam[next] == SlotSeam[slot] && SlotSeamLight[next] == SlotSeamLight[slot];
		}

		// One phase of the occlusion-culled pass for `DrawSlots`: where its
		// indirect arguments start and how many instances each slot-run may
		// draw, which is what lets a run with nothing to draw skip its binds.
		struct IndirectPhase {
			SDL_GPUBuffer *Arguments = nullptr;
			uint32_t FirstArgument = 0;
			const std::vector<uint32_t> *RunDraws = nullptr;
		};

		bool EnsureOcclusionResources(
			uint32_t argCount, uint32_t candidateCount, uint32_t runCount, uint32_t lateCount
		);
		bool EnsurePyramid(uint32_t width, uint32_t height);
		void BuildPyramid(SDL_GPUCommandBuffer *command, SDL_GPUTexture *depth);
		void DispatchOcclusionCull(SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection);
		void ReleaseOcclusion();

		// --- particles --------------------------------------------------------
		//
		// **Two pipelines and one buffer.** The two differ only in their blend
		// state - one mixes into the target and one adds to it - and blend state
		// is baked into a pipeline on every modern API, which is the same reason
		// `TransparentPipeline` is a second object rather than a uniform on the
		// first.
		//
		// **Additive is not a cosmetic variant.** Adding is commutative, so an
		// additive emitter's particles need no back-to-front sort at all; at half a
		// million particles that is the difference between sorting and not.
		SDL_GPUGraphicsPipeline *ParticlePipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrParticlePipeline = nullptr;
		SDL_GPUGraphicsPipeline *AdditiveParticlePipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrAdditiveParticlePipeline = nullptr;

		// Shared programs for every world's particle pool. The state and output
		// buffers are world-owned below; the shaders carry no world state.
		SDL_GPUComputePipeline *ParticleStep = nullptr;
		SDL_GPUComputePipeline *ParticleEmit = nullptr;
		SDL_GPUComputePipeline *ParticleScatter = nullptr;

		// One run of particles that share every uniform and every binding.
		//
		// **What makes the target count drawable at all.** One draw call per
		// emitter is a hundred thousand of them at the roadmap's scale; grouping
		// by state takes a grid of identical emitters to a single call.
		struct ParticleGroup {
			// Which batch's state this group draws with. Every batch folded into
			// it compares equal under `SameParticleState`, so any of them would
			// do and the first is the one kept.
			uint32_t Batch = 0;

			// Where this group's particles start in the shared buffer.
			uint32_t First = 0;

			// How many there are.
			uint32_t Count = 0;

			// A coarse conservative bound shared by every emitter in this material
			// run. Invalid authored motion leaves `Cullable` false and draws it.
			core::AABB Bounds;
			bool HasBounds = false;
			bool Cullable = false;
			double CullableAfter = 0.0;

			// The emitter-sized spans folded into this material run. They let a
			// camera reject particles behind it even when the aggregate box surrounds
			// the eye, while adjacent visible spans still collapse into one draw.
			uint32_t FirstSpan = 0;
			uint32_t SpanCount = 0;
		};

		struct ParticleCullSpan {
			uint32_t Batch = 0;
			uint32_t First = 0;
			uint32_t Count = 0;
			core::AABB Bounds;
			bool Cullable = false;
			double CullableAfter = 0.0;
		};

		struct ParticleDrawRun {
			uint32_t First = 0;
			uint32_t Count = 0;
		};

		struct ParticleDrawGroup {
			uint32_t FirstRun = 0;
			uint32_t RunCount = 0;
			uint32_t Culled = 0;
		};

		// --- ribbons ----------------------------------------------------------
		//
		// **The same two-pipeline shape the particles have**, and for the same
		// reason: blend state is baked into a pipeline, and an additive ribbon is
		// order-independent where a blended one is not.
		//
		// The primitive differs. A particle is a quad expanded from its vertex
		// index; a ribbon is a real vertex stream, because its geometry is a
		// function of where its endpoints are and that is resolved on the CPU
		// where a test can reach it - `ribbon.vert` carries the argument.
		SDL_GPUGraphicsPipeline *RibbonPipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrRibbonPipeline = nullptr;
		SDL_GPUGraphicsPipeline *AdditiveRibbonPipeline = nullptr;
		SDL_GPUGraphicsPipeline *HdrAdditiveRibbonPipeline = nullptr;

		SDL_GPUBuffer *RibbonBuffer = nullptr;
		SDL_GPUTransferBuffer *RibbonTransfer = nullptr;
		uint32_t RibbonCapacity = 0;

		// Grows the ribbon buffer, on `ReserveParticles`'s terms.
		bool ReserveRibbons(uint32_t count);

		// Uploads this frame's ribbon vertices, outside every render pass.
		//
		// @return How many vertices were packed.
		uint32_t PrepareRibbons(std::span<const effects::RibbonVertex> vertices);

		// Draws the runs `effects::BuildRibbons` produced.
		//
		// **One draw call per run and no grouping**, which is the opposite of the
		// particle path and is right for the opposite reason: a run is already a
		// whole beam or a whole trail, so a scene has tens of them where it has
		// tens of thousands of emitters. Grouping would save a bind on a count
		// that does not need saving, and it cannot merge two runs anyway - they
		// are separate strips, and a strip drawn as one primitive would connect
		// the end of one to the start of the next.
		//
		// @param triangles Added to, rather than set - `DrawSlots` takes the
		//        frame's running total the same way.
		// @return How many draw calls were issued.
		uint32_t DrawRibbons(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			std::span<const effects::RibbonRun> runs,
			uint64_t &triangles,
			WorldColourTarget target = WorldColourTarget::Display
		);

		// This frame's groups, and the batch order they were built from.
		//
		// Members rather than locals, so the capacity survives the frame - the
		// same argument `DrawOrder` makes one screen up.
		std::vector<ParticleGroup> ParticleGroups;
		std::vector<ParticleCullSpan> ParticleSpans;
		std::vector<uint32_t> ParticleOrder;

		// --- the device-resident pool -----------------------------------------
		//
		// **The particles live here and are simulated here.** Until v0.17 the
		// host stepped the pool into its own array and this class copied the
		// result across every frame: at half a million particles that is sixteen
		// megabytes, and `particles.pack` was the single largest thing a
		// particle frame did - larger than the simulation and two orders of
		// magnitude larger than recording the draw.
		//
		// Steady frames cross only parameter or curve records that changed. The
		// flat work list crosses when emitter layout changes.
		// `particle-step.comp` does the rest and writes its output straight into
		// its world's buffer at the offsets `PrepareParticles` worked out, so the
		// draw grouping is exactly what it was and there is no gather pass.
		struct ParticlePool {
			// One state per slot, `STATE_WORDS` wide, read and written by the
			// integration pass and written by emission. Never read by the host.
			SDL_GPUBuffer *States = nullptr;
			SDL_GPUTransferBuffer *StateStaging = nullptr;
			uint32_t Slots = 0;

			// The resident work list: one block and state-row pair per particle slot,
			// in sorted draw order. It crosses only when the layout changes, then lets
			// the device run full 64-wide groups across emitter boundaries.
			//@{
			SDL_GPUBuffer *Work = nullptr;
			SDL_GPUTransferBuffer *WorkStaging = nullptr;
			uint32_t WorkCapacity = 0;
			//@}

			// The two tables the step reads by block index, and the one staging
			// staging records that feed them.
			//
			// **Persistent, and written only where they disagree with the host.**
			// See `PARTICLE_WORK_WORDS` for why: staging every block every frame
			// is more traffic than the pool itself used to cost.
			//@{
			SDL_GPUBuffer *Params = nullptr;
			SDL_GPUBuffer *Curves = nullptr;
			SDL_GPUBuffer *EmitterRuntime = nullptr;
			SDL_GPUTransferBuffer *EmitterRuntimeStaging = nullptr;
			uint32_t TableRows = 0;

			// The records that update them, staged and scattered.
			//
			// **One pair each, and they cannot share.** A scatter record is its
			// destination row followed by the payload, so its stride *is* the
			// payload width - fifty-one words for a parameter record and
			// sixty-five for a curve one. The first version put both in one
			// buffer from its two ends and told the shader one width, so every
			// parameter update but the first landed at the wrong offset and the
			// table filled with garbage capacities. It drew, which is why it took
			// a side-by-side capture rather than a crash to find.
			//@{
			SDL_GPUBuffer *ParamUpdateBuffer = nullptr;
			SDL_GPUTransferBuffer *ParamStaging = nullptr;
			uint32_t ParamStagingRows = 0;

			SDL_GPUBuffer *CurveUpdateBuffer = nullptr;
			SDL_GPUTransferBuffer *CurveStaging = nullptr;
			uint32_t CurveStagingRows = 0;
			//@}
			//@}

			// What the device was last told about each block, indexed by block.
			//
			// **Compared against `EmitterBlock::Revision` rather than against the
			// record itself**, because comparing the record means reading both
			// copies of ninety-six bytes for every emitter every frame - which at
			// a hundred thousand of them is most of the traffic the counter exists
			// to avoid. Zero means "never told", which is what a block index
			// nobody has claimed yet reads as.
			//@{
			std::vector<uint32_t> ParamRevision;
			std::vector<uint32_t> CurveRevision;
			//@}

			// The portal panes, if the scene has any.
			//@{
			SDL_GPUBuffer *Seams = nullptr;
			SDL_GPUTransferBuffer *SeamStaging = nullptr;
			uint32_t SeamCapacity = 0;
			//@}

			// What changed for the dispatch being recorded.
			//@{
			uint32_t WorkItems = 0;
			uint32_t WorkUpdates = 0;
			uint32_t SeamCount = 0;
			uint32_t ParamUpdates = 0;
			uint32_t CurveUpdates = 0;
			float Delta = 0.0f;
			double SimulatedSeconds = 0.0;
			std::vector<ParticleCullRecord> CullRecords;
			//@}
		};

		// Particle simulation is world-owned for the same reason instance rows
		// are. Block indices start at zero inside each world, and every world's
		// generated vertex stream must survive until a batched command buffer has
		// drawn it.
		struct ParticleWorld {
			uint64_t Id = 0;
			core::Name Name;
			ParticlePool Pool;

			// One prepared scene shared by every camera for this logical world in
			// the current renderer frame. The first view stages and advances it;
			// later views reuse both its material groups and device output.
			uint64_t PreparedFrame = 0;
			uint64_t PreparedRevision = 0;
			uint64_t PreparedLayoutRevision = 0;
			uint64_t PreparedResidentRevision = 0;
			bool PreparedRevisionValid = false;
			bool ResidentRefreshPending = false;
			uint32_t PreparedCount = 0;
			std::vector<ParticleBatch> PreparedBatches;
			std::vector<ParticleGroup> PreparedGroups;
			std::vector<ParticleCullSpan> PreparedSpans;
			std::vector<uint32_t> PreparedOrder;
			bool PreparedCullingSafe = true;

			// Emitter bounds and their folded runs are host facts. Keep the last
			// camera's plan until one of those facts changes or an old-bound
			// lifetime expires.
			ParticleDrawPlanStamp DrawPlanStamp;
			std::vector<ParticleDrawGroup> DrawGroups;
			std::vector<ParticleDrawRun> DrawRuns;

			// Time recorded into the command currently owned by the host. A submit
			// failure carries it into the next device step instead of losing time.
			//@{
			float CarriedDelta = 0.0f;
			float PendingDelta = 0.0f;
			bool SubmissionPending = false;
			bool StateInitialisationPending = false;
			//@}

			// Written by this world's compute step and read as its vertex stream.
			// There is no transfer buffer: these rows never live on the host.
			SDL_GPUBuffer *Buffer = nullptr;
			uint32_t Capacity = 0;
		};
		std::vector<ParticleWorld> ParticleWorlds;
		ParticleWorld *ActiveParticleWorld = nullptr;

		ParticleWorld &ParticleWorldFor(uint64_t id, core::Name name) {
			const auto found =
				std::find_if(ParticleWorlds.begin(), ParticleWorlds.end(), [&](const ParticleWorld &world) {
					return world.Id == id && world.Name == name;
				});
			if (found != ParticleWorlds.end()) {
				return *found;
			}
			ParticleWorlds.push_back(ParticleWorld{});
			ParticleWorlds.back().Id = id;
			ParticleWorlds.back().Name = name;
			return ParticleWorlds.back();
		}

		// Grows the pool's state buffer and the staging buffers to what this frame
		// needs.
		//
		// **The state buffer is never re-created once it is big enough**, and it
		// must not be: it is the simulation. Re-creating it would empty every
		// live particle in the scene, which is a visible pop rather than a slow
		// frame.
		//@{
		bool ReserveParticlePool(uint32_t slots, SDL_GPUCommandBuffer *command);
		bool ReserveParticleTables(uint32_t blocks, SDL_GPUCommandBuffer *command);
		bool ReserveParticleStaging(uint32_t workItems, uint32_t seams);
		//@}

		// Runs changed-state uploads, emission and integration in the frame's
		// command buffer.
		//
		// **Once per world rather than once per view.** The pool is a world's and
		// not a camera's; `PrepareParticles` records it for the first view and
		// later cameras reuse the output. Recording it here removes the separate
		// submission while command order still makes the following draws see it.
		struct ParticleDispatch {
			bool Succeeded = false;
			uint32_t Count = 0;
		};
		ParticleDispatch DispatchParticles(SDL_GPUCommandBuffer *command, uint32_t timingSlot);

		void ReleaseParticlePool();

		// Grows the particle buffer to hold at least `count`, keeping what fits.
		//
		// **Grown and never shrunk**, exactly as the instance buffer is: an
		// explosion is a spike in the particle count and a frame that reallocated
		// on the way back down would pay for the spike twice.
		bool ReserveParticles(uint32_t count);

		// Groups the batches, decides where each block's run lands in the draw
		// stream, and stages the changed records and seams the device will read.
		//
		// **Outside every render pass**, because the copy that follows it is a
		// copy pass and a copy pass cannot be started while a render pass is
		// open. The first version of this did the memcpy inside the draw and
		// never issued the copy at all, so the vertex buffer held whatever the
		// previous frame left - which draws *something*, which is why it took a
		// capture rather than a crash to find.
		//
		struct ParticlePreparation {
			uint32_t Count = 0;
			uint32_t Dispatches = 0;
		};

		// @return How many instance slots and compute dispatches the frame recorded.
		ParticlePreparation
		PrepareParticles(const View &view, SDL_GPUCommandBuffer *command, uint32_t timingSlot);

		// Draws what the step wrote.
		//
		// @param triangles Added to, rather than set - `DrawSlots` takes the
		//        frame's running total the same way.
		// @return How many draw calls were issued.
		uint32_t DrawParticles(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			uint64_t &triangles,
			uint32_t &particlesDrawn,
			uint32_t &culled,
			WorldColourTarget target = WorldColourTarget::Display
		);

		// Chosen once so pipelines and depth textures use one supported format.
		SDL_GPUTextureFormat DepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;

		SDL_GPUTexture *DepthTexture = nullptr;
		uint32_t DepthWidth = 0;
		uint32_t DepthHeight = 0;

		// --- the offscreen scene target ---------------------------------------
		//
		// Where the world goes when a caller asks for a texture instead of the
		// window. See `render::SceneTarget` for why an editor needs one.
		// One offscreen target per viewport asking for one.
		//
		// **A vector rather than a single texture, and the editor is why.** Two
		// viewports are two different sizes, so one shared target would be
		// destroyed and recreated twice a frame as each panel asked for its
		// own - a colour and a depth texture per frame, which is exactly the
		// cost `RetiredScenes` exists to avoid paying even once.
		struct SceneSlot {
			static constexpr size_t RETAINED_FRAMES = 3;
			uint64_t ContentSignature = 0;
			static constexpr uint32_t NO_RETAINED_FRAME = UINT32_MAX;

			// Stable mesh and resident identities beside a retained viewport.
			// Particle-only redraws still fill object draw metadata from the ECS rows,
			// but do not resolve meshes or probe identical resident rows again.
			struct InstanceSourceRow {
				InstanceKey Key;
				const MeshEntry *Mesh = nullptr;
				uint32_t ResidentSlot = 0;
			};

			SDL_GPUTexture *Texture = nullptr;

			// What was allocated, which is the panel's size rounded up to a
			// block. See `SCENE_TARGET_BLOCK`.
			uint32_t Width = 0;
			uint32_t Height = 0;

			// The rectangle inside it the world is drawn into, which is the
			// panel's size exactly. What the pass sets its viewport to and what
			// `SceneTextureExtent` reports. See `render::SceneExtent`.
			uint32_t DrawnWidth = 0;
			uint32_t DrawnHeight = 0;

			// Per-slot depth matches the block-rounded colour target dimensions.
			SDL_GPUTexture *Depth = nullptr;
			uint32_t DepthWidth = 0;
			uint32_t DepthHeight = 0;

			// The previous completed graph output. A swapchain image is neither
			// owned by the renderer nor guaranteed to support sampling, so the
			// last-frame graph resource must never alias it.
			SDL_GPUTexture *History = nullptr;
			uint32_t HistoryWidth = 0;
			uint32_t HistoryHeight = 0;
			bool HistoryReady = false;

			// Visibility and order remain target-owned. The packed rows they index
			// are shared by every camera carrying the same world key.
			std::vector<uint32_t> InstanceIndices;
			std::vector<InstanceSourceRow> InstanceSources;
			std::vector<uint32_t> InstanceSourceOrder;
			bool InstanceSourcesReady = false;
			std::vector<uint32_t> SkinOffsets;
			std::vector<uint32_t> JointWords;
			uint64_t SkinOffsetSignature = 0;
			uint64_t JointWordSignature = 0;
			bool SkinOffsetsReady = false;
			bool JointWordsReady = false;
			bool SkinOffsetsDirty = false;
			bool JointWordsDirty = false;
			uint32_t SkinOffsetCapacity = 0;
			uint32_t JointWordCapacity = 0;
			SDL_GPUBuffer *SkinOffsetBuffer = nullptr;
			SDL_GPUTransferBuffer *SkinOffsetTransfer = nullptr;
			SDL_GPUBuffer *JointBuffer = nullptr;
			SDL_GPUTransferBuffer *JointTransfer = nullptr;

			// One acknowledged stream per possible in-flight frame. Partial copies
			// preserve unchanged device bytes, so a buffer cannot be reused until the
			// frame that last read it has retired.
			struct InstanceIndexVersion {
				SDL_GPUBuffer *Buffer = nullptr;
				SDL_GPUTransferBuffer *Transfer = nullptr;
				uint32_t Capacity = 0;
			};
			std::array<InstanceIndexVersion, IndexResidency::VERSIONS> InstanceIndexVersions;
			IndexResidency ResidentIndices;

			// Completed output images are separate from the target being written.
			// The CPU publishes one only after its submission fence signals, so an
			// interface never samples a texture the device is still replacing.
			struct RetainedFrame {
				SDL_GPUTexture *Texture = nullptr;
				uint32_t Width = 0;
				uint32_t Height = 0;
				uint32_t DrawnWidth = 0;
				uint32_t DrawnHeight = 0;
				FrameResult Result;
				uint64_t Sequence = 0;
				bool Pending = false;
			};
			std::array<RetainedFrame, RETAINED_FRAMES> Retained;
			uint32_t PublishedFrame = NO_RETAINED_FRAME;
			uint32_t NextRetainedFrame = 0;
		};

		std::vector<SceneSlot> SceneSlots;

		struct StagedSceneFrame {
			size_t Slot = 0;
			uint32_t Frame = 0;
			uint64_t Sequence = 0;
		};
		static constexpr size_t RESOURCE_IMAGE_CAPACITY = 6;
		struct PendingSceneSubmission {
			SDL_GPUFence *Fence = nullptr;
			std::vector<StagedSceneFrame> Frames;
			std::array<uint32_t, RESOURCE_IMAGE_CAPACITY> Images{};
			uint32_t ImageCount = 0;
		};

		enum class ResourceImagePhase : uint8_t { Free, Queued, Recorded, Submitted, Ready };
		struct ResourceImageSlot {
			ResourceImagePhase Phase = ResourceImagePhase::Free;
			ResourceImage Image;
			SDL_GPUTexture *Resident = nullptr;
			SDL_GPUTexture *ResidentDepth = nullptr;
			SDL_GPUTexture *ResidentNormal = nullptr;
			SDL_GPUTexture *ResidentAmbientResponse = nullptr;
			SDL_GPUTexture *ResidentLightingBaseline = nullptr;
			SDL_GPUTexture *ResidentDirectionalResponse = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t TransferBytes = 0;
			uint32_t TransferStride = 0;
			uint32_t DepthStride = 0;
			uint32_t DepthOffset = 0;
			uint32_t NormalStride = 0, NormalOffset = 0;
			uint32_t AmbientResponseStride = 0, AmbientResponseOffset = 0;
			uint32_t LightingBaselineStride = 0, LightingBaselineOffset = 0;
			uint32_t DirectionalResponseStride = 0, DirectionalResponseOffset = 0;
			bool Cancelled = false;
		};
		// One ordered capture: opaque, two physical layers, overflow and spatial overlay.
		std::array<ResourceImageSlot, RESOURCE_IMAGE_CAPACITY> ResourceImages;
		struct ResidentImagePair {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			SDL_GPUTexture *Normal = nullptr;
			SDL_GPUTexture *AmbientResponse = nullptr;
			SDL_GPUTexture *LightingBaseline = nullptr;
			SDL_GPUTexture *DirectionalResponse = nullptr;
			uint32_t Width = 0, Height = 0;
		};
		std::array<ResidentImagePair, 4> ResidentImageCache{};
		size_t NextResidentCache = 0;
		bool ReuseResidentImage(
			ResourceImageSlot &slot,
			uint32_t width,
			uint32_t height,
			bool depth,
			bool ambient = false,
			bool directional = false
		);
		void ReleaseResidentImageCache();
		uint64_t NextResourceImageToken = 1;
		struct DataCaptureSource {
			std::string SnapshotId;
			core::CFrame CameraFrame;
			scene::Camera Camera;
			bool ProjectionAvailable = false;
			std::array<float, 16> Projection{};
		};
		DataCaptureSource ActiveDataCaptureSource;
		bool HasShadowCaptureRequest(core::Name pipeline, size_t viewSlot) const;
		bool EnsureResourceImageTransfer(ResourceImageSlot &slot, uint32_t bytes);
		void RecordShadowResourceImages(
			SDL_GPUCommandBuffer *command,
			core::Name pipeline,
			core::Name node,
			size_t viewSlot,
			core::Name resource,
			const NamedTexture &source,
			const ResourceShadowCapture &metadata
		);
		void RecordResourceImages(
			SDL_GPUCommandBuffer *command,
			core::Name pipeline,
			core::Name node,
			size_t slot,
			core::Name resource,
			const NamedTexture &source,
			core::Name depthResource,
			const NamedTexture &depth,
			core::Name normalResource,
			const NamedTexture &normal,
			core::Name ambientResponseResource,
			const NamedTexture &ambientResponse,
			core::Name lightingBaselineResource,
			const NamedTexture &lightingBaseline,
			core::Name directionalResponseResource,
			const NamedTexture &directionalResponse
		);
		void CollectResourceImage(uint32_t slot);
		void ReleaseResidentImage(ResourceImageSlot &slot);
		std::vector<StagedSceneFrame> StagedSceneFrames;
		std::vector<PendingSceneSubmission> PendingSceneSubmissions;
		uint64_t NextSceneSequence = 1;

		// The slot the frame in progress is drawing into.
		size_t ActiveSlot = 0;

		SceneSlot &SlotAt(size_t slot) {
			if (SceneSlots.size() <= slot) {
				SceneSlots.resize(slot + 1);
			}
			return SceneSlots[slot];
		}

		InstanceWorld &InstanceWorldFor(uint64_t id, core::Name name) {
			const auto found =
				std::find_if(InstanceWorlds.begin(), InstanceWorlds.end(), [&](const InstanceWorld &world) {
					return world.Id == id && world.Name == name;
				});
			if (found != InstanceWorlds.end()) {
				return *found;
			}
			InstanceWorlds.push_back(InstanceWorld{});
			InstanceWorlds.back().Id = id;
			InstanceWorlds.back().Name = name;
			return InstanceWorlds.back();
		}

		void TrackInstanceUpload(const InstanceWorld &world) {
			const auto found = std::find_if(
				PendingInstanceUploads.begin(),
				PendingInstanceUploads.end(),
				[&](const PendingInstanceUpload &pending) {
					return pending.Id == world.Id && pending.Name == world.Name;
				}
			);
			if (found == PendingInstanceUploads.end()) {
				PendingInstanceUploads.push_back({world.Id, world.Name});
			}
		}

		void CompleteResidentUploads(bool submitted) {
			if (!submitted) {
				for (const PendingInstanceUpload &pending : PendingInstanceUploads) {
					InstanceWorldFor(pending.Id, pending.Name).Instances.MarkAllDirty();
				}
			}
			PendingInstanceUploads.clear();

			for (ParticleWorld &world : ParticleWorlds) {
				if (!world.SubmissionPending) {
					continue;
				}
				if (submitted) {
					world.CarriedDelta = 0.0f;
				} else {
					world.PreparedRevisionValid = false;
					world.CarriedDelta = world.PendingDelta;
					if (world.StateInitialisationPending) {
						world.Pool.Slots = 0;
					}
					std::fill(world.Pool.ParamRevision.begin(), world.Pool.ParamRevision.end(), 0);
					std::fill(world.Pool.CurveRevision.begin(), world.Pool.CurveRevision.end(), 0);
				}
				world.PendingDelta = 0.0f;
				world.SubmissionPending = false;
				world.StateInitialisationPending = false;
			}
		}

		// Scene targets that have been replaced but may still be referenced.
		//
		// Keep replaced targets alive until interface draw lists finish this frame.
		std::vector<SDL_GPUTexture *> RetiredScenes;

		// Frees what the previous frame retired. Called once at the top of a
		// frame, which is the only point at which no draw list can still name
		// one of them.
		void DrainRetiredScenes() {
			for (SDL_GPUTexture *texture : RetiredScenes) {
				gpu::ReleaseTexture(Device, texture);
			}
			RetiredScenes.clear();
		}

		// --- the frame that has been waited for but not yet recorded ----------
		//
		// Hold the claimed frame between WaitForFrame and Render.
		SDL_GPUCommandBuffer *PendingCommand = nullptr;
		SDL_GPUTexture *PendingSwapchain = nullptr;
		uint32_t PendingWidth = 0;
		uint32_t PendingHeight = 0;

		// Whether `BeginFrame` has claimed this frame and `Render` has not yet
		// consumed it.
		bool FrameClaimed = false;

		// A present mode asked for, waiting for a legal moment to be set.
		//
		// Changing present mode recreates the swapchain; apply only before acquire.
		SDL_GPUPresentMode PendingPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
		bool PresentModePending = false;

		// Sets it, if one was asked for. Safe only while no frame is claimed.
		void ApplyPresentMode() {
			if (!PresentModePending) {
				return;
			}
			PresentModePending = false;

			if (Device == nullptr || Window == nullptr) {
				return;
			}

			if (!SDL_SetGPUSwapchainParameters(
					Device, Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, PendingPresentMode
				)) {
				// **Warned rather than reported back.** The caller was told
				// whether the mode was *supported* when it asked, which is the
				// question a checkbox can act on; a driver failing the set a frame
				// later is not something there is anybody left to tell. The mode
				// is unchanged, which is the safe outcome.
				ENGINE_WARN("SDL_SetGPUSwapchainParameters: {}", SDL_GetError());
			}
		}

		// Claims this frame: drains what the last one retired, takes a command
		// buffer, and waits for a swapchain image unless there is no window.
		//
		// **Idempotent within a frame**, so a caller that waits explicitly and a
		// `Render` that would have waited for itself cannot acquire twice. That
		// matters more than it looks: two swapchain acquisitions in one frame is
		// two frames in flight consumed for one presented, which reads as the
		// frame rate halving for no reason a profile can show.
		//
		// @return `false` when there was nothing to acquire - minimised or
		//         mid-resize, which is not an error.
		bool BeginFrame() {
			if (FrameClaimed) {
				return true;
			}
			if (Device == nullptr) {
				return false;
			}
			PollSceneFrames();

			// **Before the acquire, which is the only moment it is legal.** See
			// `PendingPresentMode`.
			ApplyPresentMode();

			// **Before anything this frame records or binds.** Whatever a
			// previous frame retired is unreferenced now: its draw lists have
			// been replayed and thrown away, and nothing has yet recorded a bind
			// for this frame. Doing it here rather than in `Render` is what keeps
			// that true once the wait moved ahead of the interface - an editor
			// records its draw lists between the two calls.
			DrainRetiredScenes();

			SDL_GPUCommandBuffer *command = nullptr;
			{
				ENGINE_PROFILE_CAT("acquire command buffer", core::ProfileCategory::Render);
				command = SDL_AcquireGPUCommandBuffer(Device);
			}
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
				return false;
			}

			// **Headless waits for nothing and is not a failure.** There is no
			// swapchain to acquire and nothing to present; the frame is finished
			// when the world has been drawn into its target.
			if (Headless()) {
				PendingCommand = command;
				PendingSwapchain = nullptr;
				PendingWidth = 0;
				PendingHeight = 0;
				FrameClaimed = true;
				return true;
			}

			SDL_GPUTexture *swapchain = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
			bool acquired = false;
			{
				// Where the frame waits, and the reason this one has a span of
				// its own. "WaitAnd" is not decoration: with vertical sync on
				// this blocks until the display is ready, and with it off it
				// blocks until the GPU hands back a swapchain image. Either way
				// the time is real, the CPU is idle for it, and it is not a cost
				// anything above this can do anything about.
				//
				// A frame that looks slow with everything else on the panel
				// adding up to nothing is a frame that is waiting here - which
				// means the GPU is the limit, not the code above it.
				//
				// Idle, not Render. Nothing is being rendered here - the thread
				// is asleep until the display is ready for another image, and
				// counting that as rendering work makes the renderer look like
				// the most expensive thing in a frame it spent waiting.
				ENGINE_PROFILE_CAT("display waiting", core::ProfileCategory::Idle);
				acquired =
					SDL_WaitAndAcquireGPUSwapchainTexture(command, Window, &swapchain, &width, &height);
			}

			if (!acquired || swapchain == nullptr) {
				// Minimised, or mid-resize. Not an error, and not a reason to
				// stop ticking - the simulation carries on and the next frame
				// presents.
				//
				// **Cancelled rather than submitted, which is what SDL's own
				// example does here.** No swapchain texture was acquired, so
				// there is nothing to present and nothing recorded worth
				// executing; submitting an empty buffer sends it through the
				// whole submit path and consumes a frame in flight for no work.
				// Cancel is only legal *because* the acquire failed -
				// `SDL_CancelGPUCommandBuffer` is documented as an error once a
				// swapchain texture has been acquired, which is why every later
				// bail-out submits instead.
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			PendingCommand = command;
			PendingSwapchain = swapchain;
			PendingWidth = width;
			PendingHeight = height;
			FrameClaimed = true;
			return true;
		}

		// Hands the claimed frame to whoever is about to record it.
		void TakeFrame(
			SDL_GPUCommandBuffer *&command, SDL_GPUTexture *&swapchain, uint32_t &width, uint32_t &height
		) {
			command = PendingCommand;
			swapchain = PendingSwapchain;
			width = PendingWidth;
			height = PendingHeight;

			PendingCommand = nullptr;
			PendingSwapchain = nullptr;
			PendingWidth = 0;
			PendingHeight = 0;
			FrameClaimed = false;
		}

		// Gets rid of a frame that was claimed and will never be recorded.
		//
		// **Submitted and not cancelled**, because a swapchain texture has been
		// acquired by the time this can be reached and SDL documents cancelling
		// after that as an error. An empty submit presents nothing and costs one
		// trip through the submit path, which is the correct price for a caller
		// that waited for a frame and then decided to quit.
		void AbandonFrame() {
			if (!FrameClaimed) {
				return;
			}
			if (PendingCommand != nullptr) {
				SDL_SubmitGPUCommandBuffer(PendingCommand);
			}

			PendingCommand = nullptr;
			PendingSwapchain = nullptr;
			PendingWidth = 0;
			PendingHeight = 0;
			FrameClaimed = false;
		}

		// Where the next capture goes, or empty for none. See
		// `Renderer::RequestSceneCapture`.
		std::filesystem::path CapturePath;

		// Where the next complete host overlay goes, or empty for none.
		std::filesystem::path WindowCapturePath;

		// Which viewport's scene the pending capture wants, or `ANY_VIEWPORT`.
		//
		// **A panel per scene means the next `Render` is usually the wrong
		// one.** See `Renderer::RequestSceneCapture`: the request outlives the
		// call that made it, and honouring it in whichever call comes next
		// photographs whatever that panel happens to be showing.
		size_t CaptureSlot = Renderer::ANY_VIEWPORT;

		// A readable stand-in for the write-only swapchain on a requested Studio
		// screenshot frame. Released after its download fence signals.
		SDL_GPUTexture *WindowCaptureTexture = nullptr;
		uint32_t WindowCaptureWidth = 0;
		uint32_t WindowCaptureHeight = 0;

		bool WriteCapture(
			SDL_GPUTransferBuffer *from,
			uint32_t width,
			uint32_t height,
			SDL_GPUTextureFormat format,
			const std::filesystem::path &path
		) const;

		// --- the shadow map -------------------------------------------------
		//
		// A depth texture and the pipeline that fills it. The **same instance
		// buffer** the colour pass binds, which is what makes a shadow map one
		// more draw over data that is already on the device.
		SDL_GPUGraphicsPipeline *ShadowPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedMeshShadowPipeline = nullptr;
		SDL_GPUGraphicsPipeline *PackedShadowPipeline = nullptr;
		SDL_GPUTexture *ShadowTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

		// The beams: up to four holes' worth of shadow, in one 2x2 atlas.
		//
		// **One texture rather than four, because a fragment binds samplers and
		// not maps.** Every fragment tests every live beam, so four textures
		// would be four more samplers on every draw in the frame to serve a
		// handful of pixels near a doorway. The atlas costs one sub-rectangle per
		// beam in the uniform and one viewport per beam in the pass.
		SDL_GPUTexture *BeamTexture = nullptr;

		// What every pass pushes, whether or not anything crossed. A count of
		// zero is the ordinary case and the shader's loop ends immediately.
		BeamUniforms Beams;

		// The largest overflow already reported. Portal counts are stable for
		// most scenes, so logging the same capacity decision every frame only
		// hides later rendering diagnostics in thousands of duplicate lines.
		size_t MaximumBeamCandidatesWarned = MAX_PORTAL_BEAMS;

		// --- the surface target ----------------------------------------------
		//
		// Surface targets use colour and depth, with ping-pong textures to prevent
		// render-target self-sampling. The surface flag is per draw.
		struct SurfaceSlotState {
			SDL_GPUTexture *Texture[2] = {nullptr, nullptr};
			SDL_GPUTexture *Depth = nullptr;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Width = 0;
			uint32_t Height = 0;

			// Which of the pair this frame wrote. The other is what a surface
			// pass samples, and it holds the frame before.
			uint32_t Slot = 0;

			// Whether either texture holds a frame yet.
			//
			// The first frame has nothing to show, so a mirror draws as its own
			// tint rather than sampling whatever the driver handed back.
			bool Ready = false;

			// The frame clock when this slot last drew, for
			// `SurfaceView::FPS`.
			//
			// **Per slot rather than one stamp for the pass**, because the
			// surfaces do not refresh together and never have: one that has
			// never rendered has nothing to compare against, and one whose
			// content has not moved is skipped for a different reason entirely.
			// A shared stamp would let a mirror that redrew for its own reasons
			// reset the interval of every other mirror in the room.
			double Drawn = -1.0;

			// World to this surface camera's clip space, for the frame just
			// written. What the surface pass renders with.
			glm::mat4 ViewProjection{1.0f};

			// The same, with the pane's own map folded in. **What a pane
			// projects with, which is not the matrix the texture was rendered
			// with.** A mirror's map is the identity and the two are equal; a
			// portal's takes the pane to where its camera was fitted, and
			// without it every fragment projects outside the image. See
			// `scene::SurfaceLens::Mapping`.
			glm::mat4 Sampling{1.0f};

			// The pair again, for the frame before - which is the one another
			// surface pass samples, and it must be projected with the matrix
			// that *rendered* it. Projecting last frame's texture with this
			// frame's camera is a reflection that slides as the viewer moves,
			// and it reads as a mis-aimed camera rather than as a stale matrix.
			//@{
			glm::mat4 PreviousViewProjection{1.0f};
			glm::mat4 PreviousSampling{1.0f};
			//@}

			// How solid this surface's image is, from the view that wrote it.
			float ImageOpacity = 1.0f;

			// What the pane puts the image through when it samples it.
			//
			// **Not part of the signature**, unlike the matrix beside it: the
			// effect changes no texel of the texture, only how the screen pass
			// reads it. A mirror switched to thermal has to change this frame
			// rather than on whichever later frame something happens to move -
			// the same argument `ImageOpacity` makes one line up.
			scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

			// What the scene looked like when this surface last rendered.
			//
			// **Compared rather than trusted to a dirty bit**, for
			// `SignatureOf`'s reason: the draw list is written in bulk and
			// announces nothing. A match means this pass would redraw the
			// texture it already holds, so it is not run.
			//
			// Meaningless while `Ready` is false - a slot that has never
			// rendered refreshes on the signature it happens to hold, which is
			// why the two are always tested together.
			uint64_t Signature = 0;
		};

		// Owned foreign radiance, queued on the CPU and accepted after graph submission.
		struct ImportedPortalImage {
			uint64_t Handle = 0;
			uint64_t LayerSet = 0;
			uint8_t LayerSetCount = 0;
			PortalImageBinding Binding;
			assets::ContentHash PixelHash;
			assets::ContentHash DepthHash;
			assets::ContentHash NormalHash;
			assets::ContentHash AmbientResponseHash;
			assets::ContentHash LightingBaselineHash;
			assets::ContentHash DirectionalResponseHash;
			std::optional<uint64_t> CaptureTick;
			uint64_t ContentRevision = 0;
			uint64_t LightingRevision = 0;
			uint32_t Width = 0;
			uint32_t Height = 0;
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTexture *DepthTexture = nullptr;
			SDL_GPUTexture *NormalTexture = nullptr;
			SDL_GPUTexture *AmbientResponseTexture = nullptr;
			SDL_GPUTexture *LightingBaselineTexture = nullptr;
			SDL_GPUTexture *DirectionalResponseTexture = nullptr;
			size_t TextureBytes = 0;
			uint32_t TextureWidth = 0;
			uint32_t TextureHeight = 0;
			std::vector<std::byte> Pending;
			std::vector<std::byte> PendingDepth;
			std::vector<std::byte> PendingNormal;
			std::vector<std::byte> PendingAmbientResponse;
			std::vector<std::byte> PendingLightingBaseline;
			std::vector<std::byte> PendingDirectionalResponse;
			SDL_GPUCommandBuffer *Recorded = nullptr;
			bool Ready = false;
		};
		std::array<ImportedPortalImage, MAX_IMPORTED_PORTAL_IMAGES> ImportedPortals;
		struct ImportedPortalShadow {
			uint64_t Handle = 0;
			PortalShadowImageBinding Binding;
			std::vector<std::byte> Pending;
			SDL_GPUTexture *Texture = nullptr;
			std::vector<uint32_t> PackedPending;
			SDL_GPUBuffer *Packed = nullptr;
			SDL_GPUTransferBuffer *Staging = nullptr;
			bool DedicatedStaging = false;
			size_t GpuBytes = 0;
			SDL_GPUCommandBuffer *Recorded = nullptr;
			bool Ready = false;
		};
		std::array<ImportedPortalShadow, MAX_PORTAL_CAPTURE_TREE_NODES * 2> ImportedPortalShadows;
		ImportedPortalShadow *FindPortalShadow(uint64_t handle);
		const ImportedPortalShadow *FindPortalShadow(uint64_t handle) const;
		bool ValidPortalShadowView(const View &view, const glm::mat4 &lightProjection) const;
		bool RecordPortalShadowImport(SDL_GPUCommandBuffer *command, ImportedPortalShadow &image);
		void FinishPortalShadowImports(SDL_GPUCommandBuffer *command, bool submitted);
		void ReleasePortalShadow(ImportedPortalShadow &image);
		std::array<ImportedPortalCaptureTree, MAX_IMPORTED_PORTAL_CAPTURE_TREES> ImportedPortalTrees;
		PortalTreeCompositionJob PortalTreeJob;
		std::array<PortalPreparedTree, 2> PortalPreparedTrees;
		uint64_t NextPortalPreparation = 1;
		PortalPreparedTree *FindPortalPreparedTree(uint64_t token);
		void ReapPortalPreparedTrees(Renderer &renderer);
		void CancelPortalPreparedTrees(Renderer &renderer, uint64_t tree);
		PortalTreeCompositionStatus UploadPortalPreparedShadow(PortalPreparedTree &prepared);
		void ReleasePortalPreparedShadowAssembly(PortalPreparedTree &prepared);

		void ReleasePortalTreeShadowAssembly();
		uint64_t NextPortalTreeJob = 1;

		SDL_GPUTransferBuffer *PortalImportStaging = nullptr;
		// This buffer shares the aggregate staging budget with per-shadow buffers,
		// but its capacity is independent of their combined charge.
		size_t PortalImportStagingBytes = 0;
		PortalImageImportUsage PortalImportUsage;
		void ReportPortalImportUsage();
		void CacheResidentImage(ImportedPortalImage &image);
		void ReleasePortalImport(ImportedPortalImage &image, bool keepResidentCache = false);
		void RecordPortalImports(SDL_GPUCommandBuffer *command, const View &view, size_t viewSlot);
		void FinishPortalImports(SDL_GPUCommandBuffer *command, bool submitted);
		ImportedPortalImage *FindPortalImport(
			const View &view, size_t viewSlot, const PortalView &portal, SDL_GPUCommandBuffer *command
		);

		// One level of one portal's recursion: the picture seen through that hole
		// from the camera the level above stands at.
		//
		// **No ping-pong pair, unlike a surface slot, and the difference is what
		// makes the pass a recursion rather than an iteration.** A surface reads
		// its neighbours' textures while they read its, so a pair is the only way
		// to stop a pass sampling what another pass is writing in the same
		// bounce. A portal level is written by the recursion and read exactly
		// once, by the level above it, after that write has finished - depth-first
		// order is the ordering, so there is nothing to alternate between.
		//
		// **No signature, no rate cap and no `Ready` either.** All three are ways
		// of keeping a texture across frames, and a level's contents are only
		// meaningful for the camera that produced them - which is this frame's.
		struct PortalTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Display = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		// The pool, indexed by level and then by slot.
		//
		// **Per level per slot, and both indices are needed.** Per level alone
		// would be enough if a level were consumed the instant it was written -
		// which it is not: level `L` renders the scene and *then* draws every
		// hole visible from it, so all of level `L-1` has to survive until the
		// last of them is drawn. Per slot alone would have two levels of one hole
		// writing the same texture.
		//
		// Allocated on the frame a level is first reached and kept after, for
		// `SurfaceBank`'s reason: a viewer who walks away from a corridor of
		// holes and back again should not pay two full-screen allocations for it.
		struct PortalLevel {
			PortalTarget Targets[scene::MAX_SURFACES];
		};

		// One level of one *mirror's* recursion: the pane's picture as seen from
		// the camera the level above stands at.
		//
		// **A pane-shaped `PortalTarget`, and the two are separate for the reason
		// their passes are.** A hole's sub-render is the screen's own frustum
		// skewed, so its target is the screen's size and the pane reads the texel
		// it is standing on. A mirror's is fitted to its pane, so its target is
		// the pane's authored size and the pane reads it by projecting its own
		// world position - which is why this carries a matrix and a portal level
		// does not.
		//
		// **`Sampling` is the whole point of the type.** It is the matrix that
		// *rendered* this texture, and the level above binds it to project the
		// pane with. Before the recursion existed the pass had only the matrix
		// fitted to the eye available to it - so a pane inside another pane's
		// picture was projected from a viewpoint nobody was looking from, the
		// coordinate left 0..1, and `opaque.frag` fell back to the flat lit pane.
		// That flat slab in a mirror's reflection is the defect this exists to
		// remove.
		//
		// **No ping-pong pair and no signature**, for `PortalTarget`'s reasons: a
		// level is written by the recursion and read exactly once, by the level
		// above it, after that write has finished. Depth-first order *is* the
		// ordering, so there is nothing to alternate between and nothing worth
		// keeping across a frame - the contents are only meaningful for the
		// camera that produced them, which is this frame's.
		struct MirrorTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Width = 0;
			uint32_t Height = 0;

			// World to this level's camera clip space. What the pane above
			// projects its own world position through.
			//
			// A mirror's map is the identity - a reflection fixes every point of
			// the plane it reflects through - so this is the camera's
			// `ViewProjection` and nothing else. `scene::SurfaceLens::Mapping`
			// carries the general case for the pane path.
			glm::mat4 Sampling{1.0f};

			// Whether this frame's recursion reached this slot at this level.
			//
			// **Cleared at the top of every frame rather than trusted.** A pane
			// that went off screen, or edge-on, or whose texture could not be
			// made, leaves a target holding last frame's picture taken from a
			// camera that no longer exists - and sampling it would slide a
			// reflection across a pane nothing in the scene is moving, which is
			// the hardest possible version of this bug to attribute.
			bool Ready = false;
		};

		struct MirrorLevel {
			MirrorTarget Targets[scene::MAX_SURFACES];
		};

		// One portal mouth's light-field capture: the room its seam opens onto,
		// rendered against a lit void from a stand-in eye at the mouth.
		//
		// **The seam's geometry travels with the texture**, because the capture
		// and the projection are two passes reading one record - a projector fed
		// a rectangle the capture was not taken at throws another room's light
		// onto this one's floor, at an angle nothing authored.
		//
		// `Ready` is cleared at the top of every portal pass rather than
		// trusted, for `MirrorTarget::Ready`'s reason: a mouth that was disabled
		// or walked away from must not go on projecting last frame's rooms.
		struct SeamLightTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool Ready = false;

			// The projector, in world space: xyz centre / w unused, xyz unit
			// normal toward the lit room / w spill range, and the two half axes.
			glm::vec4 Centre{};
			glm::vec4 Outward{};
			glm::vec4 First{};
			glm::vec4 Second{};
		};

		// Every surface a viewport owns.
		//
		// **Indexed by surface number, and never released short of shutdown.**
		// A slot is allocated the first time an index is rendered and then kept,
		// which is deliberate rather than lax: the studio round-robins its
		// viewports, so one frame draws a world full of mirrors and the next
		// draws one with none. Releasing on absence would destroy and recreate
		// every surface texture on alternate frames, which is the same
		// reallocation `SCENE_TARGET_BLOCK` exists to avoid one layer up. The
		// high-water mark is bounded by `scene::MAX_SURFACES`.
		struct SurfaceBank {
			SurfaceCapturePlan CapturePlan;
			std::vector<effects::RibbonVertex> CaptureRibbons;
			std::vector<CaptureBlendSlot> CaptureBlendOrder;
			SurfaceSlotState Surfaces[scene::MAX_SURFACES];

			// **Grown to the depth actually reached**, so a world with no holes
			// in it costs one empty vector per viewport and nothing else.
			std::vector<PortalLevel> Portals;

			// The same, for mirrors. Two pools rather than one, because the two
			// passes size their targets differently - see `MirrorTarget`.
			std::vector<MirrorLevel> Mirrors;

			// The seam light-field captures, one per mouth slot. Fixed at
			// `SEAM_LIGHT_RESOLUTION` rather than pooled by level: a mouth
			// captures its far room once per frame however deep the picture
			// recursion goes.
			SeamLightTarget SeamLights[scene::MAX_SURFACES];

			// What the last frame drawn into this bank reached, which is what an
			// automatic depth reads.
			//
			// **Per viewport and not per renderer, for the reason the textures
			// are.** The studio's four panels look at one world from four
			// places, and how deep a corridor telescopes is a fact about where
			// somebody is standing - one shared number would let the panel with
			// the deepest view pay for every other panel's frame, and would
			// oscillate as the panels took turns.
			scene::SurfaceBounceProbe Bounces;
		};

		// **One bank per viewport, and that is what makes a mirror a mirror when
		// a world is on screen twice.** A reflection is of the viewer: `scene::
		// AimSurfaceCameras` mirrors the world's `ActiveCamera` through each
		// pane, so two panels looking at one world want two different images out
		// of the same `SurfaceCamera`. With one shared bank they got one - the
		// panel that drew most recently wrote every surface texture, and the
		// other panel then composited its panes from a reflection computed for
		// somebody else's eye. Flying either camera moved the mirrors in both
		// windows, at half the frame rate, which reads as a projection fault
		// rather than as one texture with two authors.
		//
		// The aim is still world state and still per frame - one panel draws per
		// frame and re-aims before it does - so what has to be per viewport is
		// the *texture*, which outlives the frame that drew it. A panel that is
		// not this frame's shows its own last image rather than another panel's
		// current one.
		//
		// **Grown on demand rather than sized to a maximum.** A client has one
		// viewport and a scene with no mirrors has no banks at all; the studio's
		// four allocate as their panels first draw a surface, and each costs a
		// texture pair per live surface index and nothing per unused one.
		std::vector<SurfaceBank> SurfaceBanks;

		SurfaceBank &SurfacesAt(size_t viewport) {
			if (SurfaceBanks.size() <= viewport) {
				SurfaceBanks.resize(viewport + 1);
			}
			return SurfaceBanks[viewport];
		}

		SDL_GPUSampler *SurfaceSampler = nullptr;

		bool EnsureShadow();
		bool EnsureAmbientComposition();
		bool EnsureDirectionalCorrection();
		bool EnsureDeferredLightingBaseline();
		bool EnsureDeferredLightingDirectional();
		SDL_GPUGraphicsPipeline *AmbientResponsePipeline = nullptr;
		SDL_GPUGraphicsPipeline *DeferredLightingBaselinePipeline = nullptr;
		SDL_GPUGraphicsPipeline *DeferredLightingDirectionalPipeline = nullptr;
		SDL_GPUGraphicsPipeline *AmbientMergePipeline = nullptr;
		SDL_GPUGraphicsPipeline *AmbientCorrectPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DirectionalCorrectPipeline = nullptr;

		// The beam atlas, made on the frame a hole first transports a shadow.
		//
		// **Shares `ShadowSampler`**, because it is the same kind of map read the
		// same way - a depth texture clamped at the edge, with the fragment
		// shader range-checking anyway.
		bool EnsureBeams();

		// The one sampler every offscreen scene texture is read through.
		//
		// **Its own call because two passes need it and only one of them used to
		// create it.** It was made inside `EnsureSurface`, which a world of
		// nothing but portals never reaches - so the portal pass captured a null
		// sampler and handed it to `SDL_BindGPUFragmentSamplers`, which
		// dereferences it. A scene with a single mirror in it hid that
		// completely.
		//
		// Linear and clamped: the clamp is what stops a fragment at the very edge
		// of a pane wrapping to the far side of the picture.
		bool EnsureSurfaceSampler();

		bool EnsureSurface(
			size_t viewport, size_t index, uint32_t width, uint32_t height, SDL_GPUTextureFormat format
		);

		// One portal level's colour and depth, at the size of the attachment the
		// level above draws into.
		//
		// **The attachment's size and not the viewport's**, which is what makes
		// `opaque.frag`'s screen-position lookup exact: the pane divides its own
		// `gl_FragCoord` by `textureSize`, so the texel it reads is the pixel it
		// is standing on only while the two rectangles are the same. A level
		// rendered at anything else would need a scale factor pushed to the
		// shader, which is a uniform that can be wrong.
		//
		// @return `false` when either texture could not be made, which drops that
		//         hole to a flat pane for the frame rather than the frame.
		PortalTarget *
		EnsurePortal(size_t viewport, uint32_t level, size_t index, uint32_t width, uint32_t height);

		// One mirror level's colour and depth, at the pane's own size.
		//
		// **The pane's size and not the viewport's**, which is the one line of
		// difference from `EnsurePortal` and follows from how the two are read.
		// A hole's picture is sampled by screen position, so it has to be the
		// screen's shape; a mirror's is sampled by projecting the pane's world
		// position through the matrix that drew it, so it can be any size at all
		// and the authored one is what the scene asked for.
		//
		// @return `null` when either texture could not be made, which drops that
		//         level to a flat pane for the frame rather than the frame.
		MirrorTarget *EnsureMirror(
			size_t viewport,
			uint32_t level,
			size_t index,
			uint32_t width,
			uint32_t height,
			SDL_GPUTextureFormat format
		);

		// One mouth's light-field capture pair, at `SEAM_LIGHT_RESOLUTION`.
		//
		// @return `null` when either texture could not be made, which loses the
		//         mouth's spill for the frame rather than the frame.
		SeamLightTarget *EnsureSeamLight(
			size_t viewport, size_t index, SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID
		);

		// One opaque white texel, bound wherever a real texture is missing.
		//
		// **The pipelines declare two fragment samplers and a draw must bind
		// both.** An unbound sampler is undefined behaviour on several backends
		// where a wrongly bound one is merely ignored, and the uniform flag is
		// what stops the result being read - so any valid texture will do, and
		// what matters is that there is always one.
		//
		// This used to be `OverlayTexture`, which is created only when a debug
		// panel has something in it, standing in for `ShadowTexture`, which is
		// created only when something casts. Both are absent together in an
		// ordinary case - a scene of nothing but transparent geometry, with the
		// panels closed - and the screen pass then bound no samplers at all and
		// drew anyway. Owning a texture for the job costs four bytes of device
		// memory and removes the case rather than making it rarer.
		SDL_GPUTexture *FallbackTexture = nullptr;

		SDL_GPUTexture *OverlayTexture = nullptr;
		SDL_GPUTransferBuffer *OverlayTransfer = nullptr;
		SDL_GPUSampler *OverlaySampler = nullptr;
		int OverlayWidth = 0;
		int OverlayHeight = 0;

		// Set when the overlay texture is created and cleared by the first
		// upload after it, which is made to cover the whole image rather than
		// only the region that changed.
		bool OverlayUninitialised = false;

		std::string Backend;

		// What this device takes, asked once at initialisation. Every shader
		// created below reads it rather than naming a format of its own.
		ShaderBinary Binary;

		SDL_GPUShader *LoadShader(
			std::string_view name,
			SDL_GPUShaderStage stage,
			uint32_t samplers,
			uint32_t uniformBuffers,
			uint32_t storageBuffers = 0
		) const;

		// A built-in compute pipeline from the same staged directory
		// `LoadShader` reads. The counts are the shader's declared bindings in
		// SDL's order; the thread counts must match its `local_size`.
		SDL_GPUComputePipeline *LoadComputePipeline(
			std::string_view name,
			uint32_t samplers,
			uint32_t readStorageBuffers,
			uint32_t writeStorageTextures,
			uint32_t writeStorageBuffers,
			uint32_t threadsX,
			uint32_t threadsY
		) const;

		bool CreatePipelines();

		// Binds a pipeline and records which family it belongs to.
		//
		// **Every opaque and transparent bind goes through this**, because
		// `DrawSlots` may substitute a variant for a run and has to know what to
		// put back. A pass that called `SDL_BindGPUGraphicsPipeline` directly
		// would leave the record saying something false, and the next run would
		// restore the wrong pipeline - a draw with somebody else's blend state,
		// which reads as a sorting bug.
		void BindPipeline(SDL_GPURenderPass *pass, SDL_GPUGraphicsPipeline *pipeline, PipelineFamily family);

		// Builds the display and HDR pipelines a named fragment shader draws through.
		//
		// Replaces whatever was registered under the name, releasing it first.
		//
		// @param name  What a material names it.
		// @param spirv The module. Must declare the sampler and uniform slots
		//              `opaque.frag` does - see `Renderer::AddShader`.
		// @return `false` when the shader or either pipeline could not be built.
		bool AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner);

		// Releases one variant's shader and pipelines.
		void DropShaderVariant(const core::Name &name, core::Name owner);

		// Releases every variant. Called from `Shutdown`.
		void ReleaseShaderVariants();

		// The variant a slot's shader means in the family currently bound.
		//
		// @return The pipeline, or null for no shader, an unknown one, or a
		//         family with no variants.
		SDL_GPUGraphicsPipeline *VariantFor(const core::Name &shader, core::Name owner) const;
		SDL_GPUGraphicsPipeline *PackedVariantFor(const core::Name &shader, core::Name owner) const;

		enum class SlotSelection : uint8_t {
			All,
			LodOnly,
		};

		// Issues the draws for one contiguous run of instance-buffer slots.
		//
		// **The loop v0.9 exists to add.** Every draw used to be one call over
		// one cube; now a run may hold several meshes and each mesh several
		// material submeshes, so this splits the run wherever the mesh or the
		// instance's texture override changes and issues a call per resulting
		// piece. The instances themselves never move - the split is entirely in
		// `first_instance` and a count.
		//
		// **Consecutive-run splitting rather than grouping.** Sorting the run by
		// mesh would produce fewer draw calls and is exactly what the blended
		// pass may not have: that order is back-to-front from the eye and
		// reordering it is the transparency bug the sort exists to prevent. One
		// rule for both passes is worth more here than a draw call.
		//
		// @param command    The frame's command buffer, for the uniform pushes.
		// @param pass       The open render pass.
		// @param first      The first slot.
		// @param count      How many slots.
		// @param lighting   The pass's uniforms, or null for a depth-only pass
		//                   that binds no samplers and pushes nothing.
		// @param shadow     The shadow map to bind, or null.
		// @param shadowSampler   Its sampler.
		// @param surface    The surface texture to bind, or null.
		// @param surfaceSampler  Its sampler.
		// @param triangles  Incremented by what was actually drawn.
		// @return How many draw calls were issued.
		uint32_t DrawSlots(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			uint32_t first,
			uint32_t count,
			const LightingUniforms *lighting,
			SDL_GPUTexture *shadow,
			SDL_GPUSampler *shadowSampler,
			SDL_GPUTexture *surface,
			SDL_GPUSampler *surfaceSampler,
			uint32_t tagFilter,
			uint64_t &triangles,
			const IndirectPhase *indirect = nullptr,
			SlotSelection selection = SlotSelection::All
		);

		// Binds mesh vertices plus the resident rows and one ordered index stream.
		void BindInstanceBuffers(
			SDL_GPURenderPass *pass,
			SDL_GPUBuffer *indices = nullptr,
			SDL_GPUBuffer *instances = nullptr,
			SDL_GPUBuffer *skinOffsets = nullptr
		);

		bool CreateGeometry();
		bool EnsureInstanceCapacity(
			uint32_t rows, uint32_t indices, bool &rowsReallocated, bool &indicesReallocated
		);
		bool EnsureDepth(uint32_t width, uint32_t height);

		// The same, into whichever depth texture the caller owns. See
		// `SceneSlot::Depth` for why a viewport keeps its own.
		bool EnsureDepthIn(
			SDL_GPUTexture *&texture,
			uint32_t &haveWidth,
			uint32_t &haveHeight,
			uint32_t width,
			uint32_t height
		);
		bool EnsureScene(uint32_t width, uint32_t height);
		bool EnsureWindowCapture(uint32_t width, uint32_t height);
		bool RetainSceneFrame(SDL_GPUCommandBuffer *command, size_t slot, const FrameResult &result);
		bool SubmitSceneCommand(SDL_GPUCommandBuffer *command);
		void DropStagedSceneFrames();
		void PollSceneFrames();
		bool EnsureHistory(size_t slot, uint32_t width, uint32_t height);

		// Whether this renderer has a window at all.
		//
		// **Headless is a device with nothing claimed**, not a hidden window. A
		// hidden window still owns a swapchain, and whether one can be acquired
		// for a window nobody can see is a per-platform answer nobody should
		// have to know. With no window there is no swapchain and no question.
		bool Headless() const {
			return Window == nullptr;
		}

		// The colour format every pipeline and the scene target are built
		// against.
		//
		// **One answer, asked in five places.** Headless has no swapchain to
		// ask, so it takes a fixed format - and the format has to be the *same*
		// fixed one everywhere, or a pipeline is built for one target and bound
		// to another. That is the whole reason this is a function rather than a
		// call to SDL at each use.
		SDL_GPUTextureFormat ColourFormat() const {
			return Headless() ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM
							  : SDL_GetGPUSwapchainTextureFormat(Device, Window);
		}
		bool EnsureOverlay(int width, int height);
	};
}
