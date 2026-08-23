#pragma once

// Everything one view's recording holds while the render graph walks it.
//
// **This is what the node handlers used to close over.** Until v0.19
// `Renderer::RenderView` was 5,485 lines: it worked the frame out, then
// registered two dozen node handlers as lambdas that captured its locals by
// reference, so every pass in the engine had to be written inside that one
// function and inside that one translation unit. The state is named here
// instead, the handlers live in `src/nodes/` one file per node family, and the
// `SDL_BeginGPURenderPass` calls that were inline in `RenderView` are inside a
// family's runner where the graph can see them. `docs/ARCH_REVIEW.md` C2 is the
// finding; `D00016` is the entry.
//
// **Named for what it is rather than `Context`.** It is the recording of one
// view: what was asked for, what the frame worked out about it, and the device
// objects the passes write into. A handler may read all of it and there is
// nothing else it is allowed to see.
//
// Private, and it has to be: it names `SDL_GPURenderPass` and
// `Renderer::Impl`, and `render/AGENTS.md` keeps both out of `include/`.

#include "RenderTypes.hpp"
#include "RendererState.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/SurfaceTable.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::render {

	// One prepared view, exactly as `Renderer::Render` handed it over.
	//
	// **A struct rather than nineteen parameters carried twice.** `RenderView`
	// took nineteen arguments and every one of them had to be threaded into the
	// recording as well; naming them once is what stops the two lists drifting.
	struct ViewRequest {
		core::CFrame CameraFrame;
		scene::Camera Camera;
		std::span<const scene::DrawInstance> Instances;
		OverlayImage *Overlay = nullptr;
		std::span<const SurfaceView> Surfaces;
		FrameOverlayHook *GameInterfaceHook = nullptr;
		FrameOverlayHook *HostOverlayHook = nullptr;
		const SceneTarget *Target = nullptr;
		size_t TargetSlot = 0;
		const View *Source = nullptr;
		std::span<const ParticleBatch> Particles;
		std::span<const effects::RibbonVertex> RibbonVertices;
		std::span<const effects::RibbonRun> RibbonRuns;
		std::span<const SceneLight> Lights;
		std::span<const scene::DrawInstance> Foreign;
		std::span<const PortalView> Portals;
		bool Present = false;
		core::Name Pipeline;
		uint64_t World = 0;
		PresentationDamage Damage;
	};

	// A surface camera this frame accepted, with what it resolved to.
	struct AcceptedView {
		size_t Index = 0;
		const SurfaceView *View = nullptr;

		// **Held here rather than written straight to the slot**, because
		// whether it may be written is not known yet. A slot's `ViewProjection`
		// has to keep describing the camera that rendered the texture the slot
		// holds - so a surface that turns out to be unchanged, and therefore
		// does not re-render, must not take this frame's matrix. See the refresh
		// decision in `ViewRecording::Begin`.
		glm::mat4 ViewProjection{1.0f};

		// The same with the pane's map folded in, which is what the pane reads
		// the image back through. `SurfaceView::Mapping` says why they are two
		// matrices rather than one.
		glm::mat4 Sampling{1.0f};

		float ImageOpacity = 1.0f;

		// What the pane puts the image through. Carried for the same reason the
		// opacity is: it is composited with, not rendered with.
		scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

		// Whether this surface renders this frame. False when its signature
		// matches the one its texture was drawn with.
		bool Refresh = true;
	};

	// The shadow map and the sampler that reads it, or the stand-ins bound where
	// the real ones do not exist. The pipelines declare both slots and a draw
	// must bind both - an unbound sampler is undefined behaviour on several
	// backends where a wrongly bound one is merely ignored.
	struct ShadowBinding {
		SDL_GPUTexture *Texture;
		SDL_GPUSampler *Sampler;
	};

	// Whether a view got as far as recording.
	enum class ViewStart : uint8_t {
		// The frame is set up and the graph may run.
		Recording,

		// Nothing to draw into, nothing allocatable, or a graph that is not
		// installed. The command buffer has already been dealt with and the
		// caller returns the result it holds.
		Abandoned,
	};

	class ViewRecording {
	  public:
		// **The renderer's device state, nameable here because `Renderer` makes
		// this class a friend.** Everything the node families record against is
		// reached through it, and spelling it once here is what keeps the
		// definition in `src/RendererState.hpp` rather than in `include/`.
		using Impl = Renderer::Impl;

		// Binds a recording to the renderer it belongs to.
		//
		// @param owner  The renderer, for `CurrentLighting`.
		// @param state  Its device objects. Borrowed for the call.
		// @param result What the view reports back. Written throughout.
		ViewRecording(Renderer &owner, Impl &state, FrameResult &result)
			: Owner(owner), State(&state), Result(result) {}

		// Works the frame out: the target, the draw list, the plan, the surface
		// and portal budgets, the uploads and the device targets every pass
		// writes into.
		//
		// **Everything here used to be the first sixteen hundred lines of
		// `RenderView`.** It is separated from the passes because it is the half
		// that decides and they are the half that records.
		//
		// @param request What to draw.
		// @return Whether the graph may run. `Abandoned` has already ended the
		//         command buffer.
		ViewStart Begin(const ViewRequest &request);

		// Runs the compiled graph over the registered handlers, then serves the
		// capture, the host chrome and the submit.
		//
		// @param nodes The assembled backend table.
		// @return The view's result, which is the same object `Result` names.
		void Finish(const NodeTable &nodes);

		// --- what the passes read ------------------------------------------

		Renderer &Owner;
		Impl *State = nullptr;
		FrameResult &Result;
		ViewRequest Request;

		// The installed graph this view is drawing.
		const Impl::NamedPipeline *Pipeline = nullptr;

		// The buffer every pass records into, and the image it presents to.
		SDL_GPUCommandBuffer *Command = nullptr;
		SDL_GPUTexture *Swapchain = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;

		// Where the world goes. `SceneWidth`/`SceneHeight` is the rectangle the
		// world fills; `TargetWidth`/`TargetHeight` is the attachment holding
		// it, which is block-rounded and usually bigger. See `SCENE_TARGET_BLOCK`.
		bool Offscreen = false;
		uint32_t SceneWidth = 0;
		uint32_t SceneHeight = 0;
		uint32_t TargetWidth = 0;
		uint32_t TargetHeight = 0;

		// The camera every projection in this frame is built from, with the
		// portal near plane already applied.
		// arch-waiver ecs-copy: the camera this recording was built from, with the
		// portal near plane already applied - so it is not the world's camera any
		// more, and a projection built from the world's would be a different one.
		scene::Camera DrawCamera;
		float NearestPane = 0.0f;
		glm::mat4 CameraMatrix{1.0f};
		scene::CameraMatrices Matrices;
		glm::mat4 LightViewProjection{1.0f};
		core::AABB SceneBounds;

		// The draw list after `scene::KeepLoaded`, and the other worlds' rows
		// behind it.
		std::span<const scene::DrawInstance> Instances;
		std::span<const scene::DrawInstance> Foreign;

		// Every range the three scene passes submit. See `scene::ScenePlan`.
		scene::ScenePlan Plan;
		uint32_t OwnCount = 0;
		uint32_t SceneCount = 0;
		size_t SceneOpaque = 0;
		uint32_t SceneTransparent = 0;
		uint32_t SceneReflected = 0;
		uint32_t ReflectedCasters = 0;
		uint32_t SurfaceCasters = 0;

		// The camera range's own partition: plain opaque, then the mirrors, then
		// the blended tail with its own mirrors last.
		size_t OpaqueCount = 0;
		uint32_t PlainOpaque = 0;
		uint32_t SurfaceInCamera = 0;
		uint32_t TransparentCount = 0;
		uint32_t PlainTransparent = 0;
		uint32_t TransparentSurfaces = 0;
		uint32_t InstanceCount = 0;
		uint32_t UploadCount = 0;
		scene::SurfaceRun CameraRuns[scene::MAX_SURFACES];

		bool HaveInstances = false;
		bool HaveOverlay = false;
		bool UploadOverlay = false;
		bool HaveShadow = false;
		bool OcclusionCulling = false;

		// This viewport's surfaces, and not the renderer's. See
		// `Impl::SurfaceBanks`.
		Impl::SurfaceBank *Bank = nullptr;

		AcceptedView Accepted[scene::MAX_SURFACES];
		size_t AcceptedCount = 0;
		bool Claimed[scene::MAX_SURFACES] = {};
		bool WantSurface = false;
		uint64_t SurfaceSignature = 0;
		size_t RefreshCount = 0;
		core::Vector3 SceneEye;
		double FrameSeconds = 0.0;

		// The holes, by slot, claimed before the surfaces are.
		const PortalView *PortalOf[scene::MAX_SURFACES] = {};
		bool HavePortals = false;
		uint32_t PortalLevels = 0;

		// The panes, by slot, for the levels below the first.
		scene::SurfacePane Panes[scene::MAX_SURFACES];
		bool HavePanes[scene::MAX_SURFACES] = {};
		bool AnyPane = false;
		uint32_t PaneWidth[scene::MAX_SURFACES] = {};
		uint32_t PaneHeight[scene::MAX_SURFACES] = {};

		// How deep the mirrors go this frame, and what the descent found.
		uint32_t SurfaceBounces = 1;
		uint32_t MirrorLevels = 0;
		bool MirrorHistory = true;
		bool SurfaceVisible[scene::MAX_SURFACES] = {};
		float SurfaceCoverage[scene::MAX_SURFACES] = {};
		scene::SurfaceBounceProbe SurfaceDepth;

		// The world's lighting, and the blocks the passes push from it.
		scene::WorldLighting CurrentLighting;
		LightUniforms SceneLights;
		LightingUniforms Lighting;
		FrameUniforms Frame;
		PbrUniforms Uniforms;

		// The material head's targets and how big each of its images is.
		Impl::PbrDimensions PbrDimensions;
		Impl::PbrSlot *Pbr = nullptr;

		// What the world, the window and the depth are drawn onto. Mutated as
		// the graph proceeds: the first pass to reach a target clears it and
		// everything after loads.
		SDL_GPUColorTargetInfo ColourTarget{};
		SDL_GPUColorTargetInfo WindowTarget{};
		SDL_GPUDepthStencilTargetInfo DepthTarget{};
		SDL_GPUTexture *ViewTarget = nullptr;
		SDL_GPUViewport SceneViewport{};
		SDL_Rect SceneScissor{};
		SDL_GPUSampler *Sampler = nullptr;

		// The sampler sets the shading nodes bind, built once because their
		// contents are this frame's textures.
		std::array<SDL_GPUTextureSamplerBinding, 1> DepthBindings{};
		std::array<SDL_GPUTextureSamplerBinding, 7> LightingBindings{};
		std::array<SDL_GPUTextureSamplerBinding, 1> TonemapBindings{};

		// The interface hooks, once each has agreed to draw. `Prepare` opens a
		// copy pass, so it has to have run before any render pass is open.
		bool DrawInterface = false;
		bool DrawHostOverlay = false;

		// The particles and ribbons packed for this frame.
		uint32_t ParticleCount = 0;
		uint32_t RibbonCount = 0;

		// What the `capture` node claimed, served after the graph has run.
		Impl::NamedTexture AuthoredCapture;
		std::filesystem::path AuthoredCapturePath;
		core::Name AuthoredCaptureNode;
		bool AuthoredCaptureOnce = false;

		// The per-node timing record. `TimingSlot` moves when a dispatch takes
		// its own command buffer, which is why it is not const.
		uint32_t TimingSlot = 0;
		core::Name TimedName;
		uint32_t OpenedMark = 0;
		SDL_GPUCommandBuffer *TimedCommand = nullptr;
		std::chrono::steady_clock::time_point OpenedWall;
		bool MainGpuWorkRecorded = false;
		bool DedicatedComputeSubmitted = false;
		std::unordered_map<uint32_t, double> CpuNodeWall;

		// --- what the passes call ------------------------------------------

		// The first enabled node of a kind in the selected graph.
		//
		// @param kind The node kind.
		// @return The node, or null when the graph has none enabled.
		const graph::Node *GraphNode(core::Name kind) const;

		// Whether the selected graph has an enabled node of a kind.
		//
		// @param kind The node kind.
		// @return `true` when one is enabled.
		bool GraphEnabled(core::Name kind) const;

		// Where the schedule placed a node.
		//
		// @param id The node.
		// @return Its scheduled entry, or null.
		const graph::ScheduledNode *ScheduledFor(graph::NodeId id) const;

		// Closes the open timing span, recording its wall time and its GPU
		// marks. Safe to call when nothing is open.
		void ClosePass();

		// Opens a timing span for an authored node, closing whatever was open.
		//
		// @param name            The authored node name, which is what the
		//                        profiler and `FrameResult::Nodes` report.
		// @param recordedCommand The buffer this node records into, or null for
		//                        the frame's own.
		void EnterNamedPass(core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr);

		// Ends a node that did its work on the CPU before any pass opened, and
		// attributes the time the entity stage already measured for it.
		//
		// @param context What the graph decided this invocation is.
		// @return Always `true`; a CPU node cannot fail the frame here.
		bool FinishCpuNode(const graph::RunContext &context);

		// Records the frame's staged uploads, once. Every node that draws
		// instances, ribbons or the overlay calls it first.
		//
		// @return `false` when a map or copy pass failed, which
		//         fails the frame rather than drawing from a stale buffer.
		bool RecordUploads();

		// Builds the per-draw lighting block from world lighting and the camera
		// a pass is drawing from.
		//
		// **Fog is eye-relative**, so a reflected or portal sub-view must not
		// reuse the screen eye even though every other authored term is shared.
		//
		// @param worldLighting Which world's lighting to read.
		// @param eye           Where this pass is looking from.
		// @param surfaceMode   `Flags.z`: which branch `opaque.frag` takes.
		// @param imageOpacity  `Flags.w`: how far a projected image is faded in.
		// @return The block.
		LightingUniforms LightingFrom(
			const scene::WorldLighting &worldLighting,
			const core::Vector3 &eye,
			float surfaceMode,
			float imageOpacity
		) const;

		// `LightingFrom` against this world's own lighting.
		//
		// @param eye          Where this pass is looking from.
		// @param surfaceMode  `Flags.z`.
		// @param imageOpacity `Flags.w`.
		// @return The block.
		LightingUniforms LightingAt(const core::Vector3 &eye, float surfaceMode, float imageOpacity) const;

		// The shadow map and its sampler, or the stand-ins.
		//
		// **Asked at each pass rather than resolved once for the frame**:
		// `State->SurfaceSampler` is made by whichever pass first needs it, and
		// a world of nothing but holes never reaches `EnsureSurface`.
		//
		// @return Both, never null.
		ShadowBinding ShadowBindings() const;

		// Opens a scene pass onto a colour/depth pair and primes it with the
		// state every scene pass shares: the clear, the light set, the beams,
		// the opaque pipeline and the one buffer pair every mesh lives in.
		//
		// @param colour      What the world is drawn into.
		// @param depth       Its depth attachment.
		// @param cycle       Whether to ask for fresh backing storage. A surface
		//                    slot is written and sampled in the same frame so it
		//                    cycles; a portal target is not.
		// @param viewport    The rectangle of the target the world fills, or
		//                    null for the whole of it. The scissor follows it.
		// @param passLights  The light set, pushed once for the pass.
		// @param clearColour Overrides the fog clear. The seam light captures
		//                    clear to the world's ambient - the lit void.
		// @return The open pass.
		SDL_GPURenderPass *OpenScenePass(
			SDL_GPUTexture *colour,
			SDL_GPUTexture *depth,
			bool cycle,
			const SDL_GPUViewport *viewport,
			const LightUniforms &passLights,
			const SDL_FColor *clearColour = nullptr
		);

		// The world minus every pane, drawn into whatever pass is open.
		//
		// @param pass          The open pass.
		// @param plainLighting Its per-draw lighting.
		// @param filter        The tag filter this view draws through.
		void DrawWorldInto(SDL_GPURenderPass *pass, const LightingUniforms &plainLighting, uint32_t filter);

		// The blended tail, minus the panes in it.
		//
		// @param pass          The open pass.
		// @param frame         The vertex block for the plain draws.
		// @param plainLighting Their lighting.
		// @param filter        The tag filter.
		// @param panesFollow   Whether the caller submits blended panes straight
		//                      after, which is what decides whether the
		//                      transparent pipeline still has to be bound.
		void DrawBlendedInto(
			SDL_GPURenderPass *pass,
			const FrameUniforms &frame,
			const LightingUniforms &plainLighting,
			uint32_t filter,
			bool panesFollow
		);

		// One fullscreen triangle into a colour target, named as a graph node.
		//
		// @param name         The authored node name, for the timing span.
		// @param pipeline     What to draw with.
		// @param target       Where to draw.
		// @param passWidth    The viewport width.
		// @param passHeight   The viewport height.
		// @param bindings     The fragment samplers.
		// @param passUniforms Fragment set 0, or null.
		// @param passLights   Fragment set 1, or null.
		// @param clear        The clear colour.
		void Fullscreen(
			core::Name name,
			SDL_GPUGraphicsPipeline *pipeline,
			SDL_GPUTexture *target,
			uint32_t passWidth,
			uint32_t passHeight,
			std::span<const SDL_GPUTextureSamplerBinding> bindings,
			const PbrUniforms *passUniforms,
			const LightUniforms *passLights,
			SDL_FColor clear
		);

		// The renderer-owned texture a named resource role resolves to.
		//
		// @param resource The authored resource name.
		// @param slot     Which viewport's copy of it.
		// @return The texture, or an invalid one when the role has none here.
		Impl::NamedTexture FixedTexture(core::Name resource, size_t slot) const;

		// The texture backing a graph resource, allocating it when asked.
		//
		// @param resource     The resource.
		// @param selectedSlot Which viewport's copy.
		// @param make         Whether to allocate one that does not exist.
		// @return The texture, or an invalid one.
		Impl::NamedTexture ResourceTexture(graph::ResourceId resource, size_t selectedSlot, bool make);

		// `ResourceTexture` for the viewport this node names, or this view's.
		//
		// @param resource The resource.
		// @param context  What the graph decided this invocation is.
		// @param make     Whether to allocate one that does not exist.
		// @return The texture, or an invalid one.
		Impl::NamedTexture
		GraphTexture(graph::ResourceId resource, const graph::RunContext &context, bool make);

		// The fragment samplers for everything a node reads, in read order. An
		// absent image binds the fallback texel rather than nothing.
		//
		// @param context What the graph decided this invocation is.
		// @return The bindings.
		std::vector<SDL_GPUTextureSamplerBinding> TextureBindings(const graph::RunContext &context);

		// Copies one image into another through the image pipeline.
		//
		// @param source          What to read.
		// @param target          What to write.
		// @param load            Whether the target clears or loads first.
		// @param reverseSpectrum Whether a single-channel source is coloured the
		//                        other way round, for the resource previews.
		// @return `false` when either image is absent, which the caller reports.
		bool DrawImage(
			const Impl::NamedTexture &source,
			const Impl::NamedTexture &target,
			SDL_GPULoadOp load,
			bool reverseSpectrum = false
		);

		// Composites a premultiplied overlay image onto a target.
		//
		// @param source What to read.
		// @param target What to write.
		// @param load   Whether the target clears or loads first.
		// @return `false` when either image is absent.
		bool DrawOverlayImage(SDL_GPUTexture *source, const Impl::NamedTexture &target, SDL_GPULoadOp load);

		// Fills the occlusion image with "nothing is occluded", for a graph that
		// lights without having authored an `ssao` node.
		void ClearOcclusion();

		// --- the node families ---------------------------------------------
		//
		// **One registration function per family, and each of them owns its
		// render passes.** A node whose pass is opened anywhere else is
		// invisible to the graph, which is the failure `render/AGENTS.md`
		// records and this layout exists to make impossible: there is nowhere
		// else to put one. Each is defined in its own file under `src/nodes/`,
		// which is what lets the module's passes compile in parallel.
		//
		// @param nodes The table to register into. It has to outlive neither
		//              this recording nor the graph run, and in practice both
		//              belong to the same `Renderer::RenderView` call.

		// `upload-instances`, and the CPU stages that resolve before it.
		void RegisterUploadNodes(NodeTable &nodes);

		// `shadow`, which is the sun's map and the portal beam atlas beside it.
		void RegisterShadowNodes(NodeTable &nodes);

		// `mirror-capture` and `mirror-overlay`: what a pane shows, and the
		// panes composited back over the frame.
		void RegisterMirrorNodes(NodeTable &nodes);

		// `portal-capture`, `portal-tonemap` and `portal-overlay`: the
		// recursion through a hole, its display copy, and the mouths drawn over
		// the frame.
		void RegisterPortalNodes(NodeTable &nodes);

		// `gbuffer` and `transparent`: the material head and the ordered tail.
		void RegisterGeometryNodes(NodeTable &nodes);

		// `last-frame`, `depth-linearise`, `hzb`, `ssao`, `deferred-lighting`
		// and `tonemap`: the screen-space chain over the material head.
		void RegisterShadingNodes(NodeTable &nodes);

		// `raster` and `dispatch`: the two nodes a pipeline document can author
		// a shader into.
		void RegisterAuthoredNodes(NodeTable &nodes);

		// `interface`, `overlay`, `present`, `viewer`, `capture` and
		// `output-image`: everything that composes or hands out the finished
		// image.
		void RegisterOutputNodes(NodeTable &nodes);

	  private:
		// Converts the frame's draw list into device rows and stages them.
		//
		// **A phase of `Begin` rather than a node**, because nothing in it
		// touches the device queue: it fills the mapped transfer buffer and
		// builds the occlusion plan, and `RecordUploads` records their copies.
		void PackInstances();

		// Ends a view that cannot draw: the batch owner drops its downloads, and
		// a lone view submits what it has so the buffer is not leaked.
		void EndIncompleteView();

		// Whether the frame's uploads have already been recorded.
		bool UploadsRecorded = false;
	};
}
