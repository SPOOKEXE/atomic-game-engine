#pragma once

// Client renderer. GPU types remain private to the implementation.
//
// @tier L12 · client

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Shader.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/assets/TextureSequence.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/EngineGraph.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/render/FrameResult.hpp>
#include <engine/render/ImageGraphBinding.hpp>
#include <engine/render/SceneLight.hpp>
#include <engine/render/StartupProgress.hpp>
#include <engine/render/SurfaceView.hpp>
#include <engine/render/View.hpp>
#include <engine/render/capture/DataCapture.hpp>
#include <engine/render/capture/Readback.hpp>
#include <engine/render/capture/SceneCapture.hpp>
#include <engine/render/capture/VisibilityObservation.hpp>
#include <engine/render/interface/FrameOverlayHook.hpp>
#include <engine/render/interface/Overlay.hpp>
#include <engine/render/pipeline/Capabilities.hpp>
#include <engine/render/pipeline/GraphRunner.hpp>
#include <engine/render/pipeline/PipelineAdmission.hpp>
#include <engine/render/portal/PortalCaptureTreeCompose.hpp>
#include <engine/render/presentation/PresentationDamage.hpp>
#include <engine/render/resources/Flipbook.hpp>
#include <engine/render/resources/ResourceImage.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>
#include <engine/scene/environment/CloudDensity.hpp>
#include <engine/scene/environment/GpuParticleField.hpp>
#include <engine/scene/environment/Sunlight.hpp>
#include <engine/scene/environment/VectorField.hpp>
#include <engine/script/ComputeBridgeFwd.hpp>

// `SurfaceView::Projection` is a matrix. glm has always arrived here through
// `core/types/CFrame.hpp` and `graph/Frustum.hpp`, but a header that names a
// type should say where it comes from rather than rely on a neighbour.
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct SDL_Window;

namespace engine::graph {
	class PipelineDocument;
}

namespace engine::imagegraph {
	struct Document;
	struct Diagnostic;
}

namespace engine::render {
	struct TextureBatchImage;
	class DataFactoryHookBind;
	struct PackedMeshData;
	struct MeshVertexPatch;
	enum class MeshCopyStatus : uint8_t;
	enum class TextureBandStatus : uint8_t;
	enum class TextureCopyStatus : uint8_t;
	class ShaderLibrary;
	struct PortalImageBinding;
	struct PortalShadowImageBinding;
	struct PortalShadowImage;
	struct PortalImageReply;
	struct PortalImageLayerSet;
	struct PortalImageCapture;
	struct PortalCaptureTree;
	struct ImportedPortalCaptureTree;
	struct PortalCaptureLenses;
	struct PortalImageImportUsage;

	// Owns the client GPU device, window claim, pipelines, and per-frame upload resources.
	//
	// @client
	class Renderer {
	  public:
		// A bounded, device-free description of one installed render graph.
		struct RenderGraphSnapshot {
			// Stable name of the installed pipeline.
			core::Name Pipeline;
			// Revision of the installed graph description.
			uint64_t Revision = 0;
			// Authored graph copied for inspection.
			graph::RenderGraph Graph;
			// Validated graph used to schedule node execution.
			graph::CompiledGraph Compiled;
			// Dependency order selected for execution.
			graph::ExecutionSchedule Schedule;
			// Reuse plan for transient graph resources.
			graph::ResourceAliasPlan Aliases;
			// Runtime profile recorded for this pipeline.
			graph::PipelineProfile Profile;
		};
		// The installed identity used by render-owned one-shot work. Invalid and
		// missing view names resolve through the same fallback as Render.
		struct PipelineIdentity {
			// Resolved installed pipeline name.
			core::Name Name;
			// Revision of that pipeline when the work was admitted.
			uint64_t Revision = 0;
		};
		// Creates an uninitialised renderer with no GPU resources.
		Renderer();

		// Shuts down the renderer and releases all GPU resources.
		~Renderer();

		// Renderers cannot share ownership of a device or claimed window.
		Renderer(const Renderer &) = delete;

		// Renderers cannot share ownership of a device or claimed window.
		Renderer &operator=(const Renderer &) = delete;

		// Creates the device and claims the window. Returns false and logs the
		// reason; the caller decides whether that is fatal, because a headless
		// test run legitimately has no GPU.
		//
		// The renderer does not own `window`; it must remain alive until Shutdown
		// or destruction releases the GPU claim.
		//
		// **A null window is headless, and is not an error.** There is then no
		// swapchain, nothing is presented, and the overlay and interface passes
		// do not run - but the world is still drawn, into the `SceneTarget` a
		// caller passes to `Render`. That is what makes a build server, a golden
		// image comparison and a scripted editor possible without a display,
		// and a headless `Render` with no scene target draws nothing rather than
		// pretending to.
		//
		// **How many frames the CPU may queue ahead of the GPU is a policy, and
		// the caller owns it.** One means `SDL_SubmitGPUCommandBuffer` blocks
		// until the GPU has finished the previous frame - the CPU and the GPU
		// take turns, the picture is as close to the input as this engine can
		// make it, and a GPU-bound scene loses the rate the overlap was buying.
		// Two or three let the CPU run ahead and cost a frame or two of latency
		// for it.
		//
		// **The default is one because this is an editor's renderer**, and the
		// argument is in `Initialise` beside the call. A caller measuring
		// throughput, or presenting something nobody is dragging, should say so
		// rather than have this decided for it.
		//
		// @param window SDL window to claim, or null for headless.
		// @param framesInFlight How many frames the CPU may queue ahead.
		//        Clamped to 1..3; ignored when headless, which has no swapchain
		//        to be ahead of.
		// @param retainSourceTextures Keep bounded decoded base pixels for factory export.
		// @param textureBudgetBytes Content texture capacity. Zero keeps the normal
		//        `TextureTable::MAXIMUM_BYTES` safety limit.
		// @param progress Optional owner-thread progress pump; false cancels startup.
		//        Must not call renderer methods while compilation is running.
		// @return True when the device, pipelines and geometry are ready.
		bool Initialise(
			SDL_Window *window,
			uint32_t framesInFlight = 1,
			bool retainSourceTextures = false,
			size_t textureBudgetBytes = 0,
			const StartupProgressCallback &progress = {}
		);

		// An owned copy of the last successfully submitted visibility snapshot.
		// A submission is not a claim that pixels survived depth or blending.
		VisibilitySnapshot Visibility() const;
		// Effective pass gates for the last scene recording in this viewport.
		PassActivity ViewPassActivity(size_t slot = 0) const;

		// Whether this renderer has a window to present to.
		//
		// @return `true` when `Initialise` was given none.
		bool IsHeadless() const;

		// Waits for GPU work, releases the window claim and resources, and becomes uninitialised.
		//
		// Calling this on an uninitialised renderer has no effect.
		void Shutdown();

		// Queues owned, authenticated linear RGBA16F pixels for the portal-capture
		// node. Returns zero on refusal. Changed pixels or fitted-view revisions
		// invalidate the previous handle; identical radiance avoids another upload.
		uint64_t QueuePortalImage(const PortalImageBinding &binding, PortalImageReply &&reply);
		// Immutable copied D32 source map. Zero means refused; readiness follows submission.
		uint64_t QueuePortalShadowImage(const PortalShadowImageBinding &binding, PortalShadowImage &&image);
		// Lossless source depth retained in a storage buffer, decoded by the native shadow node.
		// Subnormal samples use raw D32 storage because fragment output can flush them.
		// Shares import CPU/GPU budgets; refusal preserves the input.
		uint64_t
		QueuePackedPortalShadowImage(const PortalShadowImageBinding &binding, PortalShadowImage &&image);
		// Reports whether an accepted shadow image has finished device upload.
		bool IsPortalShadowImageReady(uint64_t handle) const;
		// Retires a shadow-image handle and its resident resources.
		bool DropPortalShadowImage(uint64_t handle);

		// Queue a complete copied set into new slots, retaining previous imports.
		// Handles are base first, then ordered transparent layers. Refusal preserves
		// input and handles; optional resident cache entries may be evicted for allocation.
		bool QueuePortalImageLayerSet(
			const PortalImageBinding &binding, PortalImageLayerSet &&layers, std::span<uint64_t> handles
		);
		// True only after every member of this exact base handle's set was submitted.
		// Dropping any member with DropPortalImage retires the entire set.
		bool PortalImageLayerSetReady(uint64_t base) const;

		// Imports an authenticated tree into independent groups. Zero means refusal;
		// older trees survive, but the supplied pixel storage may have been consumed.
		uint64_t QueuePortalCaptureTree(const PortalImageBinding &root, PortalCaptureTree &&tree);
		// Queue-safe readiness: every node's uploads were successfully submitted.
		// This does not claim GPU fence completion or nested body composition parity.
		bool PortalCaptureTreeReady(uint64_t token) const;
		// Borrowed until tree retirement or renderer shutdown; null before readiness.
		const ImportedPortalCaptureTree *FindPortalCaptureTree(uint64_t token) const;
		// Independent display/candidate ownership, bounded to two live leases per renderer.
		// Leases keep the original charged images, without extending transport deadlines.
		// The lease owner must hard Drop on endpoint or authorization retirement.
		uint64_t AcquirePortalCaptureTreeLease(uint64_t token);
		// Releases one independently owned tree lease.
		void ReleasePortalCaptureTreeLease(uint64_t lease);
		// Release the original import owner. Hard Drop revokes every lease as well.
		void ReleasePortalCaptureTree(uint64_t token);
		// Revokes the import owner and all leases for a captured tree.
		void DropPortalCaptureTree(uint64_t token);

		// Compose destination-space opaque body rows against an accepted room group.
		// The view supplies only body geometry, its palettes and a caller-owned target;
		// camera and lighting come from the capture. World and slot must match its owner.
		// Returns an owned resident image for aperture sampling, or zero on refusal.
		// Reuses the preceding composed image for this binding; DropPortalImage retires it.
		uint64_t ComposePortalBodyImage(const PortalImageCapture &capture, const View &body);
		// Composes current body through an admitted tree, returning an owned root image.
		// Child cameras must clip their mapped entrance. Failure preserves existing images.
		uint64_t ComposePortalCaptureTree(uint64_t token, const View &rootBody);
		// Copies body geometry, palettes and content-owner mappings; captured lighting replaces
		// caller lights. One job owns at most the tree metadata byte limit in copied work.
		// Unsupported borrowed inputs are refused. Tree retirement cancels its job.
		// Pending with no request waits for submitted GPU work. BudgetExceeded can be retried.
		// Invalid responses preserve the pending request; only Poll transfers a completed image.
		PortalTreeCompositionStatus
		BeginPortalCaptureTreeComposition(uint64_t tree, const View &rootBody, uint64_t &job);
		// Transfers completed composition progress and its image once.
		PortalTreeCompositionProgress PollPortalCaptureTreeComposition(uint64_t job);
		// Supplies an owned source shadow image to a waiting tree job.
		PortalTreeCompositionStatus AcceptPortalCaptureTreeShadow(uint64_t job, PortalShadowImage &&image);
		// The host authenticates the manifest envelope. Assembly bytes share all portal import
		// CPU admission; mismatched metadata is refused before allocation. One node assembles at a time.
		PortalTreeCompositionStatus
		BeginPortalCaptureTreeShadowAssembly(uint64_t job, const PortalShadowSnapshot &manifest);
		// The final tile attempts composition. BudgetExceeded retains the complete map; Commit
		// retries without replaying tiles. Only Poll transfers a completed composition image.
		PortalTreeCompositionStatus
		AcceptPortalCaptureTreeShadowTile(uint64_t job, uint32_t node, std::span<const std::byte> packet);
		// Retries composition from a complete assembled shadow map.
		PortalTreeCompositionStatus CommitPortalCaptureTreeShadowAssembly(uint64_t job, uint32_t node);
		// Cancels pending requests and owned work for a composition job.
		void CancelPortalCaptureTreeComposition(uint64_t job);

		// Two independently leased preparations admit every source shadow before a pose is copied.
		// Poll Complete means every map upload has finished; no image is transferred.
		// Requests carry accepted eye identity. The source's declared native shadow domain is
		// checked during preparation; exact all-node body/domain agreement is checked at pose Begin.
		PortalTreeCompositionStatus
		BeginPortalCaptureTreePreparation(uint64_t tree, const View &referenceBody, uint64_t &preparation);
		// Reports upload completion and next missing shadow request.
		PortalTreeCompositionProgress PollPortalCaptureTreePreparation(uint64_t preparation);
		// Supplies a complete source shadow image to a preparation.
		PortalTreeCompositionStatus
		AcceptPortalPreparedShadow(uint64_t preparation, PortalShadowImage &&image);
		// The preparation equivalents assemble one authenticated source map before its
		// ordered GPU upload. They use the reference body's exact per-node fit.
		PortalTreeCompositionStatus BeginPortalCaptureTreePreparationShadowAssembly(
			uint64_t preparation, const PortalShadowSnapshot &manifest
		);
		// Adds an authenticated tile to the preparation's current source map.
		PortalTreeCompositionStatus AcceptPortalPreparedShadowTile(
			uint64_t preparation, uint32_t node, std::span<const std::byte> packet
		);
		// Admits a fully assembled source map without replaying its tiles.
		PortalTreeCompositionStatus CommitPortalPreparedShadowAssembly(uint64_t preparation, uint32_t node);
		// Cancels one shadow-preparation lease and its pending uploads.
		void CancelPortalCaptureTreePreparation(uint64_t preparation);
		// Reuses prepared GPU maps with one latest-pose snapshot and one final queue fence.
		// Existing composition Poll/Cancel APIs own completion; source maps remain reusable.
		PortalTreeCompositionStatus
		BeginPreparedPortalCaptureTreeComposition(uint64_t preparation, const View &rootBody, uint64_t &job);

		// Invalidates one owned image at an owning-thread frame boundary.
		bool DropPortalImage(uint64_t handle);
		// Reports current CPU and GPU budgets used by imported portal images.
		PortalImageImportUsage PortalImageUsage() const;

		// Releases GPU residency owned by one world while keeping shared content
		// such as meshes and images available to the remaining worlds.
		//
		// @param world The process-local world identity.
		// @param name The stable world name used by the presentation cache.
		void ForgetWorld(uint64_t world, core::Name name);

		// Registers a mesh under the name a `DrawInstance` will ask for.
		//
		// **The one door content comes in through**, and it is on the renderer
		// rather than on the table so that `MeshTable` stays an implementation
		// detail behind the pimpl like everything else here. A caller holding a
		// `delivery::Asset` reads it into an `assets::MeshData` and hands it
		// over; nothing about the device reaches them.
		//
		// Registering a name twice for one owner replaces it. The old geometry
		// becomes reusable after the table's deferred-frame window.
		// Asset APIs use an optional content owner. Empty owner selects the shared
		// namespace; scoped lookups never search another owner. Built-in asset
		// lookups always select shared residency, matching their draw bindings.
		//
		// @param name The name to publish it under.
		// @param mesh The geometry. An invalid one is refused.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `false` for an invalid mesh, a full table or a failed upload.
		bool AddMesh(const core::Name &name, const assets::MeshData &mesh, core::Name owner = {});
		// Monotonically changes when one exact resident mesh is replaced. Zero
		// means the requested owner has no mesh under this name.
		uint64_t MeshRevision(const core::Name &name, core::Name owner = {}) const;
		// Retires one mesh in `owner` without releasing that owner's unrelated resources.
		// Its storage follows MeshTable's deferred-frame reuse rule.
		bool DropMesh(const core::Name &name, core::Name owner = {});
		// Rewrites part of one resident mesh's vertices in place, see
		// `MeshTable::Patch`. Sent with the next frame's residency upload.
		bool PatchMesh(const core::Name &name, const MeshVertexPatch &patch, core::Name owner = {});
		// Uploads validated, prepacked mesh streams under one content owner.
		bool AddPackedMesh(const core::Name &name, const PackedMeshData &mesh, core::Name owner = {});
		// Copies a bounded resident mesh for a host-owned export. This never reads
		// back the GPU or substitutes the fallback mesh for a missing name.
		MeshCopyStatus CopyMesh(
			const core::Name &name,
			assets::MeshData &out,
			size_t vertexLimit,
			size_t indexLimit,
			core::Name owner = {}
		) const;
		// Copies a bounded decoded base level for exact owner-scoped export.
		// This does not read back GPU pixels or substitute another owner's image.
		TextureCopyStatus CopyTexture(
			const core::Name &name, assets::TextureData &out, size_t byteLimit, core::Name owner = {}
		) const;

		// Retires meshes, textures, material/lens/postprocess variants and pending texture
		// arrivals for one owner. Empty owner preserves shared resources.
		void DropContentOwner(core::Name owner);

		// A registered mesh's own half-extent, in mesh space.
		//
		// **So an editor can make `Size` mean the mesh's proportions.** Since
		// `Size` is a box the mesh is stretched into, a part whose box has the
		// wrong shape distorts whatever is put in it - and the only thing that
		// knows the right shape is the geometry. `render::MeshEntry::Extent`
		// carries the whole argument.
		//
		// @param name The mesh.
		// @param out  Set only when the mesh is registered.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `false` for a name this table does not hold, so a caller can
		//         tell "not loaded yet" from "flat on one axis".
		bool MeshExtentOf(const core::Name &name, core::Vector3 &out, core::Name owner = {}) const;

		// Registers a texture under the name a `SurfaceAppearance` or a submesh
		// will ask for.
		//
		// @param name  The name to publish it under.
		// @param image The pixels. An invalid one is refused.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `false` for an invalid image, a full table or a failed
		//         upload.
		bool AddTexture(const core::Name &name, const assets::TextureData &image, core::Name owner = {});

		// `AddTexture` spread over calls; see `TextureTable::AddBand`.
		TextureBandStatus AddTextureBand(
			const core::Name &name, const assets::TextureData &image, size_t bandBytes, core::Name owner = {}
		);

		// Installs a bounded authored graph and exact resident source bindings. A refused
		// installation preserves the previous graph and every published output. Mesh
		// attribute nodes must already be lowered to data sources (`ImageGraphMeshes.hpp`);
		// wide precision and blur need rgba16 storage images.
		bool SetImageGraph(
			core::Name owner,
			core::Name name,
			const imagegraph::Document &document,
			std::span<const ImageGraphSourceBinding> sources,
			imagegraph::Diagnostic &diagnostic
		);

		// Evaluates only the selected dirty dependency cone before consumers record.
		// Publishes an ordinary texture in `owner`; unchanged calls submit no work.
		// Same-size updates preserve TextureHandle. Refusal retains accepted pixels.
		// A graph too large to keep resident runs as one job at a time, a slice of pixel
		// work per call, freeing each intermediate after its last reader; those calls
		// return Pending.
		ImageGraphEvaluation EvaluateImageGraph(
			core::Name owner,
			core::Name name,
			std::string_view output,
			core::Name publishedTexture,
			imagegraph::Diagnostic &diagnostic
		);

		// The content-addressed source the last Pending evaluation of this graph waits for,
		// or an invalid name when it waits for nothing.
		core::Name ImageGraphNeededSource(core::Name owner, core::Name name) const;

		// Releases this graph's intermediates and outputs without touching its sources.
		bool DropImageGraph(core::Name owner, core::Name name);

		// Polls completed timestamp queries without waiting and returns traffic totals.
		ImageGraphStatistics ImageGraphProfile();

		// Says that content is on its way under this name, and that it is not.
		//
		// **The fact the renderer cannot learn for itself.** It knows what it
		// holds; what is in flight belongs to the content pump, which is a layer
		// above. Between these two calls a drawable naming that texture draws as
		// the default material rather than as the purple marker - so a scene
		// load looks like untextured parts becoming textured, instead of a
		// purple shimmer indistinguishable from forty misspellings.
		//
		// **`StopExpecting` goes on the request *finishing*, not on it
		// succeeding.** A host that unmarks only on arrival leaves a misspelled
		// name expected for ever, which is exactly the case the marker exists
		// for. An arrival needs no call at all: `AddTexture` clears it.
		//
		// @param name What was asked for.
		// @param owner The exact content namespace, or empty for shared content.
		// @since v0.13
		//@{
		void ExpectTexture(const core::Name &name, core::Name owner = {});
		void StopExpectingTexture(const core::Name &name, core::Name owner = {});
		//@}

		// Whether content is on its way under this name.
		//
		// @param name The name.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `true` between the two calls above.
		// @since v0.13
		bool ExpectingTexture(const core::Name &name, core::Name owner = {}) const;

		// How long animation has been running, for anything played on a clock.
		//
		// **The caller's clock, because this module holds none** - the rule the
		// whole engine keeps. A client passes its own accumulated seconds and so
		// does the studio; a paused editor simply stops advancing it, which is
		// what makes a paused world's GIFs hold their frame with no second
		// mechanism for it.
		//
		// @param seconds Seconds since the session began.
		// @since v0.10
		void SetAnimationTime(double seconds);

		// Large sequences preserve cell dimensions and durations in one bounded atlas.
		bool AddTextureSequence(core::Name name, assets::TextureSequenceData sequence, core::Name owner);

		// Compiles and installs a named render graph for a view to select.
		//
		// The device path executes the node kinds its backend implements. The
		// engine's PBR path is a default graph, not a required set of stages. A
		// graph that asks for another node the backend does not submit is refused
		// instead of producing a partial frame.
		// The previous graph of the same name remains installed after a refusal.
		//
		// @param name     The process-local key a view will select.
		// @param pipeline Copied and compiled before installation.
		// @return Whether the complete graph can run on this backend.
		bool SetPipeline(core::Name name, const graph::RenderGraph &pipeline);

		// Compiles and installs a graph while retaining the precise refusal reason.
		// The bool overload above remains for callers that only need acceptance.
		PipelineAdmissionResult SetPipelineWithResult(core::Name name, const graph::RenderGraph &pipeline);

		// Builds and installs an authored document with the same diagnostics as
		// direct graph admission.
		PipelineAdmissionResult SetPipelineDocument(core::Name name, const graph::PipelineDocument &document);

		// Checks an authored document against this renderer without installing it.
		PipelineAdmissionResult ValidatePipelineDocument(const graph::PipelineDocument &document) const;

		// Installs, or replaces, the native implementation of a custom node kind.
		//
		// RegisterNodeKind must have declared the kind first. Built-in handlers
		// cannot be replaced through this extension door. The handler survives
		// Shutdown and is available again after Initialise invokes its lifecycle.
		//
		// @param kind The catalogue kind to implement.
		// @param handler Work performed when the graph reaches the node.
		// @param lifecycle Optional device resource ownership hooks.
		// @return False for an undeclared or built-in kind, an empty handler, or a
		//         failed reinstall against the current device.
		bool InstallNodeHandler(core::Name kind, NodeHandler handler, NodeHandlerLifecycle lifecycle = {});

		// Removes one named graph. A view naming it uses the engine default graph.
		bool RemovePipeline(core::Name name);

		// Every installed graph key, sorted by text.
		std::vector<core::Name> Pipelines() const;

		// Copies an installed graph, its compiled schedule, and its device-free
		// dimensioned profile for a read-only host diagnostic.
		std::optional<RenderGraphSnapshot>
		DescribePipeline(core::Name name, uint32_t viewWidth, uint32_t viewHeight) const;

		// Resolves a requested graph name through the same fallback as Render.
		std::optional<PipelineIdentity> ResolvePipelineIdentity(core::Name requested) const;

		// Removes every named graph.
		void ResetPipelines();

		// Starts or stops a nonblocking preview download of one graph resource.
		// Four-byte colour resources also receive a histogram. Every allocated PBR
		// image is available through `ResourceTexture` without a readback stall.
		//
		// @param resource Which authored resource, or invalid to stop.
		// @param slot     Which renderer target owns the image, or `ANY_VIEWPORT`.
		void Inspect(core::Name resource, size_t slot = ANY_VIEWPORT);

		// Which resource is currently being previewed.
		//
		// @return The authored resource `Inspect` was last given, or an empty
		//         `Name` when nothing is being downloaded.
		core::Name Inspecting() const;

		// What one observed frame did with one render node.
		struct PipelineNodeRun {
			// Authored node name.
			core::Name Node;
			// Ran, or Failed when its handler refused.
			ang::NodeStatus Status = ang::NodeStatus::Idle;
			// CPU recording time, from the start of the observed frame.
			double BeginMilliseconds = 0.0;
			double Milliseconds = 0.0;
		};

		// One frame of an observed pipeline, in execution order. A node that runs
		// per view or per world appears once for each.
		struct PipelineRun {
			core::Name Pipeline;
			std::vector<PipelineNodeRun> Nodes;
			double Milliseconds = 0.0;
		};

		// Records every execution of one installed pipeline through the engine
		// executor's hooks. An invalid name stops, and costs nothing per node.
		void ObservePipeline(core::Name pipeline);

		// The last frame that finished for the observed pipeline.
		const PipelineRun &ObservedRun() const;

		// One completed preview download.
		struct ReadbackImage {
			// The authored resource and renderer target this image came from.
			//@{
			core::Name Source;
			size_t Slot = ANY_VIEWPORT;
			//@}
			// The image's size in pixels.
			//@{
			uint32_t Width = 0;
			uint32_t Height = 0;
			//@}

			// The pixels themselves, borrowed from the renderer's staging
			// memory. **Valid until the next `Render`**, which is why this is a
			// span rather than a vector: a copy per frame for a panel that
			// usually is not open would be the whole cost of the feature.
			std::span<const uint32_t> Pixels;

			// The four channels' distributions, filled for four-byte colour
			// resources and left empty for anything else.
			ImageHistogram Histogram;

			// How many frames old this image is. A preview is deliberately
			// allowed to lag rather than stall the GPU, so a panel showing one
			// says how far behind it is.
			uint64_t Age = 0;

			// Whether this holds an image at all.
			//
			// @return `true` when the size is non-zero and pixels are present.
			bool IsValid() const {
				return Width > 0 && Height > 0 && !Pixels.empty();
			}
		};

		// Returns the last completed preview without waiting for the GPU.
		ReadbackImage Readback() const;

		// Queue one image from an enabled capture node, without waiting for the
		// device. Six requests/results may be held. Resident colour captures are
		// bounded to 512x512; copied pixels use the shared 64 MiB staging bound.
		// Directional shadow captures use PORTAL_SHADOW_EXTENT. A full queue refuses
		// before recording.
		// The node must run before a result exists. File captures remain independent.
		bool RequestResourceImage(const ResourceImageRequest &request);

		// Admit one to six captures together, or leave the queue unchanged. All
		// requests must use one pipeline, view and delivery mode, with distinct tokens.
		// This reserves queue slots only; publication must still validate every result.
		bool RequestResourceImages(std::span<const ResourceImageRequest> requests);

		// Assign a renderer-local token and queue atomically. Zero means refusal.
		uint64_t QueueResourceImage(
			core::Name pipeline,
			core::Name node,
			size_t viewSlot = 0,
			ResourceImageDelivery delivery = ResourceImageDelivery::CopiedPixels,
			std::string expectedSnapshotId = {}
		);

		// Generate tokens and admit the whole capture group. Refusal preserves outputs.
		bool QueueResourceImages(
			core::Name pipeline,
			std::span<const core::Name> nodes,
			size_t viewSlot,
			ResourceImageDelivery delivery,
			std::span<uint64_t> tokens
		);

		// Check successful resident submission and exact extent before publishing a
		// receipt. This neither waits for the GPU nor transfers capture ownership.
		bool CanPublishResourceImage(uint64_t token, uint32_t width, uint32_t height) const;

		// Move a submitted resident capture into this renderer's portal ownership.
		// Ordered GPU submissions make it usable without a CPU readback or wait.
		// Zero refuses without consuming the capture. No device handle crosses hosts.
		uint64_t AdoptResourceImage(uint64_t token, const PortalImageBinding &binding);

		// Adopt one capture frame as a group, preserving captures, previous imports
		// and output handles on refusal. Tokens and destination owners must be
		// distinct; all captures must share pipeline, view, frame and extent.
		bool AdoptResourceImages(
			std::span<const uint64_t> tokens,
			std::span<const PortalImageBinding> bindings,
			std::span<uint64_t> handles
		);

		// Take only this caller's completed request when several producers share a renderer.
		std::optional<ResourceImage> TakeResourceImage(uint64_t token);

		// Take a bounded copied group in token order only when every result is ready.
		// Missing, duplicate, resident or pending tokens consume nothing. Failed
		// captures are returned too, so callers can retire a refused layer set.
		std::optional<std::vector<ResourceImage>> TakeResourceImages(std::span<const uint64_t> tokens);

		// Poll completed scene fences and move out owned image bytes exactly once.
		std::vector<ResourceImage> TakeResourceImages();

		// Suppress delivery. A submitted copy keeps its slot until its fence signals.
		bool CancelResourceImage(uint64_t token);

		// Queues every requested render-graph capture atomically. The named node
		// must be an enabled `capture` node; unsupported channels stay explicit in
		// the later poll rather than being filled with invented labels.
		bool QueueDataCapture(const DataCaptureRequest &request, DataCaptureTicket &ticket);

		// Returns Pending until every underlying GPU fence has completed. Ready
		// planes own tightly packed bytes and BLAKE3-256 hashes, so they remain
		// immutable after the renderer reuses its transfer buffers.
		DataCapturePoll PollDataCapture(DataCaptureTicket &ticket);

		// Cancels all outstanding resource transfers named by a ticket.
		void CancelDataCapture(DataCaptureTicket &ticket);

		// Render-owned hook state. Script and world adapters use it through their
		// implementation files; no hook type crosses those public boundaries.
		DataFactoryHookBind &Hooks();
		// Borrows render-owned hook state for inspection.
		const DataFactoryHookBind &Hooks() const;

		// GPU execution time and CPU command-recording wall time for each
		// physical pass, in microseconds and keyed by Name::Id. GPU results lag
		// until the query pool resolves; wall time is from the latest Render.
		//@{
		const std::unordered_map<uint32_t, double> &PassTimings() const;
		const std::unordered_map<uint32_t, double> &PassWallTimes() const;
		//@}

		// Sequence number of the frame whose GPU times PassTimings shows. It only
		// grows, when a newer frame's timestamps resolve, so a caller can take
		// each resolved frame's GPU totals exactly once.
		uint64_t PassTimingSequence() const;

		// Whether this backend can produce GPU timestamps.
		bool Timed() const;

		// Selects per-node instrumentation for subsequent frames.
		//
		// Must be changed between frames on the owning thread.
		void SetProfiling(ProfilingTier tier);

		// The current runtime profiling tier.
		ProfilingTier Profiling() const;

		// Timestamp mark writes omitted from the latest frame because its query
		// budget filled. A non-zero value means GPU timing rows are partial.
		size_t DroppedProfileMarks() const;

		// The immutable feature snapshot probed during Initialise.
		//
		// Before initialisation every feature is unavailable.
		const DeviceCaps &Capabilities() const;

		// How many times the surface pass runs per frame.
		//
		// **In-frame recursion, and since v0.15 it is a real one.** A surface pass
		// draws the *other* panes, and with one level it draws them flat: the
		// only camera it has for them is the one placed from the eye, and that
		// camera is not the one this picture is being taken from. The coordinate
		// leaves the texture and `opaque.frag` falls back to the plain lit pane,
		// which is the blank slab a mirror seen in a mirror used to show.
		//
		// This used to be a count of times to run the pass again, which fixed the
		// staleness `D00112` was about and could never fix the viewpoint -
		// iterating refreshes textures and never moves a camera. It is now the
		// number of levels of a depth-first recursion, each level's camera being
		// `scene::ReflectCamera` applied to the level above, so reflections
		// compose. One level is the surface pass itself; the rest are the
		// recursion's.
		//
		// **The meaning of a given number is unchanged**: two has always meant
		// "two levels of mirror-in-mirror" and still does. What changed is that
		// the second level is now drawn from where it is actually seen from.
		//
		// **Superlinear in visible panes**, which the iterating version was not: a
		// level descends into every *other* pane it can see, so the passes go as
		// `panes × (panes - 1) ^ (levels - 1)` before `graph::VisiblePane` takes
		// them back down. That is why there is a ceiling now - see
		// `MAX_SURFACE_DEPTH`.
		//
		// **Zero is `scene::AUTOMATIC_SURFACE_BOUNCES` and is the default.** It
		// used to mean "no surface pass at all", which nobody ever wanted and
		// which has a clearer spelling. What it means now is that each viewport
		// measures the frame it just drew - how deep the chain of panes actually
		// went, and whether the deepest level still had a pane in view it was not
		// allowed to descend into - and draws one level deeper next frame when
		// there was and one shallower when the depth was not being used. So a
		// corridor of facing panes deepens itself to the ceiling and a room with
		// one mirror in it costs one level, with nobody having stated anything.
		// `scene::NextSurfaceBounces` is the rule and is where the argument for
		// it being free of oscillation lives.
		//
		// **A stated number overrides the measurement outright**, and there are
		// two of them: a world's `workspace.SurfaceBounces`, which is the one a
		// scene author should reach for, and `--surface-bounces` on the client
		// and the studio, which is a session overriding the world. Both arrive
		// here, so this call does not know or care which it got. A stated number
		// is floored at one and clamped at `MAX_SURFACE_DEPTH`.
		//
		// **A pane with no rectangle keeps the old behaviour**, because nothing
		// can reflect a camera through a pane it was never told about: a surface
		// camera parented to the world, or one showing a second world, still
		// resolves whatever chain it has by iterating -
		// `scene::DEFAULT_SURFACE_BOUNCES` of them, since there is nothing there
		// to measure. See `SurfaceView::PaneNormal`.
		//
		// @param bounces How many levels to resolve, or zero to measure it.
		// @since v0.15
		void SetSurfaceBounces(uint32_t bounces);

		// How many surface panes a viewport draws at once.
		//
		// **A budget, and the frame keeps the panes covering the most screen.**
		// A pane costs a whole render of the world into a texture, so the count
		// is the second thing after the depth that decides what a hall of
		// mirrors costs - `SetSurfaceBounces` is how *deep* and this is how
		// *many*. When more panes are visible than this allows, the ones drawn
		// are the ones with the largest share of the screen, as
		// `graph::VisibleSurfaces` measured it; the rest keep whatever texture
		// they last drew, so a mirror that drops out of the budget goes stale
		// rather than blank.
		//
		// **Ranked rather than taken in list order**, and that is the whole
		// design. Dropping the last few views in the order they arrived makes
		// which mirrors work a property of what order the level was built in,
		// which is invisible to the author and changes when they move something.
		// Coverage is a property of where the player is standing, which is the
		// thing they are actually judging.
		//
		// **Clamped to what there is storage for.** `scene::MAX_SURFACES` bounds
		// the slot arrays; a world asking for more is drawn at what the renderer
		// has, exactly as an over-ambitious `SetSurfaceBounces` is.
		//
		// **Zero draws no surfaces at all**, which is a legitimate low-detail
		// setting and is not a special value meaning anything else.
		//
		// The world states it as `workspace.MaxSurfaces` and
		// `scene::SurfaceLimitOf` reads it; a host passes that through here, the
		// same path `SurfaceBounces` takes.
		//
		// @param panes How many surface panes to draw per frame.
		// @since v0.17
		void SetSurfaceLimit(uint32_t panes);

		// What it is set to, which is `scene::DEFAULT_SURFACE_LIMIT` before a
		// host says otherwise.
		//
		// **Not how many drew last frame** - that is per viewport and a function
		// of what was on screen. This is the setting, for `SurfaceBounces`'
		// reason.
		//
		// @since v0.17
		uint32_t SurfaceLimit() const;

		// Draws every opaque and blended instance as lines rather than filled
		// triangles.
		//
		// **A developer's view of the geometry rather than a game feature.**
		// Reflections, portals and shadows are unaffected on purpose - a
		// shadow map is depth-only and has no fill mode to speak of, and a
		// mirror showing the wireframe of the room it reflects is still the
		// room's wireframe, which is the point rather than a gap.
		//
		// **A part wearing its own `ShaderScript` keeps it.** This overrides
		// the engine's own shading, not an author's; see `Renderer.cpp`'s
		// `BindPipeline` for where that line is drawn.
		//
		// **Silently unavailable on a device with no `fillModeNonSolid`
		// feature**, rather than a call that could fail. The two pipelines
		// this needs are built once, at `Initialise`, and either both exist
		// or the toggle does nothing - every ordinary frame renders exactly
		// as it would have regardless.
		//
		// @param enabled Whether to draw wireframe from here on.
		// @since v0.18
		void SetWireframe(bool enabled);

		// Whether wireframe was asked for.
		//
		// **Not whether the device can actually do it** - a caller wanting to
		// know that reads `Statistics()` and finds every triangle count at
		// its ordinary value on a frame this asked for wireframe and did not
		// get it. This is the setting, for `SurfaceBounces`' own reason.
		//
		// @return The last value passed to `SetWireframe`, or `false` before
		//         the renderer has a device.
		// @since v0.18
		bool Wireframe() const;

		// Draws every instance with the default white texture rather than its
		// own, from here on.
		//
		// **For looking at shapes rather than at surfaces**, which is the
		// collider view's whole problem: a wireframe drawn over a textured
		// scene is a wireframe over a photograph, and the shape somebody opened
		// the view to check is the thing they cannot pick out of it. With the
		// images gone, what is left is the lighting on the geometry - enough to
		// read form, quiet enough that an overlay sits on top of it.
		//
		// **A binding and not a pipeline**, which is the difference from
		// `SetWireframe`: a fill mode is baked into a pipeline and had to
		// become a second family, and a texture is chosen per draw. So this
		// costs one branch in `DrawSlots` and works on every device.
		//
		// The data maps go with the colour map. A normal map on a flat white
		// surface is the one that still reads, and reading it is what makes a
		// shape hard to see.
		//
		// @param enabled Whether to ignore instance textures from here on.
		// @since v0.19
		void SetUntextured(bool enabled);

		// Whether untextured drawing was asked for.
		//
		// @return The last value passed to `SetUntextured`, or `false` before
		//         the renderer has a device.
		// @since v0.19
		bool Untextured() const;

		// What it is set to: zero for automatic, and zero before the renderer has
		// a device.
		//
		// **Not what the last frame resolved**, which is per viewport and lives
		// in the bank that drew it. This is the setting.
		//
		// @since v0.15
		uint32_t SurfaceBounces() const;

		// How deep the recursive portal pass goes.
		//
		// **Not `SurfaceBounces` under another name, and the two measure
		// different things.** A bounce runs the whole surface pass again to let
		// one frame's textures propagate between panes; a portal *level* is a
		// scene rendered from a sub-camera derived from the level above it, with
		// its own cull and its own target. Depth two is a hole seen through a
		// hole, resolved inside the frame, from the right viewpoint at each
		// level.
		//
		// **Costs one scene render per visible portal per level**, which is
		// `portals ^ depth` in the worst case and is exactly why
		// `graph::VisiblePane` is asked at every level rather than once for the
		// eye. CodeParade's demo affords four because its portals are the whole
		// scene; ours share a frame with mirrors, shadows and a world.
		//
		// **One by default, and that is a backend limit rather than a budget.**
		// Above one, a level's target is rendered into once per hole at the level
		// above - so within one command buffer the same texture is written,
		// sampled, and written again, and SDL's Vulkan backend hangs the device on
		// it. The default's comment in the source carries the measurement. Raising
		// this draws correctly and is not currently safe to ship at.
		//
		// Zero draws every pane flat and runs no sub-render, which is the
		// termination case made reachable rather than a separate switch.
		//
		// @param depth How many levels. Clamped to `MAX_PORTAL_DEPTH`.
		// @since v0.15
		void SetPortalDepth(uint32_t depth);

		// What it is set to, or zero before the renderer has a device.
		//
		// @since v0.15
		uint32_t PortalDepth() const;

		// Sets every built-in lighting term for the world drawn next.
		//
		// The value is copied. A renderer holds no pointer into a world, and a
		// caller presenting several worlds sets each one's value immediately
		// before its frame.
		//
		// @param lighting The resolved `Lighting` service.
		// @since v0.16
		void SetLighting(const scene::WorldLighting &lighting);

		// The complete lighting state currently used for a view.
		scene::WorldLighting CurrentLighting() const;

		// Which way the world's one directional light shines, and what reaches
		// what it does not.
		//
		// **A knob rather than an argument to `Render`, for `SetPortalDepth`'s
		// reason**: it is a property of the world being drawn rather than of one
		// frame, and threading it through would put it in a signature every host
		// calls to repeat the same two values every frame.
		//
		// A renderer that is never told draws with `scene::SUN_DIRECTION` and
		// `scene::SUN_AMBIENT`, which are the numbers this engine has always
		// drawn with - so a host that does not care is not made to care.
		//
		// **The direction is what the shadow map is fitted along as well as what
		// the shader shades with**, so setting it moves both together. That is
		// the whole reason it is one call.
		//
		// @param direction Where it shines towards. Normalised here; a zero
		//                  vector is ignored rather than obeyed.
		// @param ambient   What reaches a surface the sun does not.
		// @param direct    The colour of the directional contribution.
		// @since v0.15
		void SetSun(
			const core::Vector3 &direction,
			const core::Color3 &ambient,
			const core::Color3 &direct = core::Color3{1.0f, 1.0f, 1.0f}
		);

		// The current directional-light settings, for a temporary nested view
		// that must restore the enclosing world's lighting.
		//@{
		core::Vector3 SunDirection() const;
		core::Color3 SunAmbient() const;
		core::Color3 SunColor() const;
		//@}

		// The backend handle for a registered texture, for an interface pass to
		// sample.
		//
		// **A `void *` for `SceneTexture`'s reason, written there in full**: the
		// header must not name an `SDL_GPUTexture`, because that would put the
		// backend's type in the interface every consumer of this header
		// compiles against. A caller that draws it already knows which backend
		// it is talking to - `ImGui::Image` takes the same opaque handle.
		//
		// **For an editor's thumbnail and not for the draw path.** The renderer
		// resolves its own textures by name inside the frame; this exists so a
		// panel can put a picture of one in a list, which is a thing only a tool
		// does.
		//
		// @param name The name it was registered under.
		// @param owner The exact content namespace, or empty for shared content.
		// @return The handle, or nullptr for a name this renderer has not been
		//         given.
		// @since v0.10
		void *TextureHandle(const core::Name &name, core::Name owner = {}) const;

		// Whether TextureHandle's RGB is decoded from sRGB during hardware sampling.
		// Missing textures return false, with the same exact owner lookup as the handle.
		bool TextureSamplesSRGB(const core::Name &name, core::Name owner = {}) const;

		// Where a texture's current animation cell sits.
		//
		// **For an interface painter, which has a name and no table.** The
		// geometry node reads the same thing from the table directly; this is the
		// same answer for the two callers outside this module.
		//
		// @param name    The texture.
		// @param seconds How long animation has been running.
		// @param owner   The exact content namespace, or empty for shared content.
		// @return The transform, or the identity for a still or an absent name.
		// @since v0.10
		FlipbookCell TextureCell(const core::Name &name, double seconds, core::Name owner = {}) const;

		// A signature of the current cells of all registered animated textures.
		//
		// This lets a presentation scheduler distinguish a still visual state from
		// one whose pixels changed only because a resident flipbook advanced.
		// Static textures contribute nothing.
		//
		// @since v0.19
		uint64_t TextureAnimationSignature(double seconds) const;

		// Changes when registered render resources are admitted, replaced or removed.
		// Scoped to this renderer lifetime; callers also track their world/endpoint identity.
		uint64_t ResourceRevision() const;

		/// Whether submitted weather evolution still needs a completion poll.
		/// A zero world selects all worlds owned by this renderer.
		bool HasPendingWeatherWork(uint64_t world = 0) const;

		// How big a registered texture is, in source pixels.
		//
		// **Handed out with the handle, because an interface painter needs
		// both.** A nine-sliced or tiled `ImageLabel` is laid out in source
		// pixels - its slice insets are in them - so a resolver returning a
		// handle alone makes every slice the wrong size, which reads as a
		// corrupt image rather than as a missing measurement.
		//
		// @param name   The name.
		// @param width  Set to the width, or left alone when the name is absent.
		// @param height Set to the height, likewise.
		// @param owner  The exact content namespace, or empty for shared content.
		// @return `false` for a texture this renderer does not hold.
		// @since v0.10
		bool
		TextureSize(const core::Name &name, uint32_t &width, uint32_t &height, core::Name owner = {}) const;

		// Forgets a registered texture and frees it.
		//
		// **Because a thumbnail cache has to have a ceiling.** Every other
		// texture here is content that lives as long as the session; a preview
		// is built for a row somebody scrolled past, and a table that only ever
		// grew would hold a store's worth of images in video memory by the time
		// somebody had browsed it.
		//
		// @param name The name to drop.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `false` for a name this renderer does not hold.
		// @since v0.10
		bool DropTexture(const core::Name &name, core::Name owner = {});

		// Builds an authored `Pass` for drawing over the lit image, reading the
		// named `images` in binding order; `compute` for SPIR-V of a `Compute`
		// pass. Same rules for replacing and refusing as `AddShader`.
		bool AddPassShader(
			const core::Name &name,
			std::span<const uint32_t> spirv,
			core::Name owner = {},
			std::span<const core::Name> images = {},
			bool compute = false
		);

		// Whether a pass of that name is ready.
		bool HasPassShader(const core::Name &name, core::Name owner = {}) const;

		// Registers a fragment shader under a name a draw instance can select.
		//
		// **The words are already SPIR-V.** Where they came from is the caller's
		// business and there are two answers: `render::ShaderLibrary` compiled a
		// `ShaderScript`'s GLSL, or it read a built-in `glslc` produced during
		// the build. Neither is this class's concern - it builds the pipelines a
		// `scene::DrawInstance::Shader` naming this resolves to.
		//
		// **The module must declare what `opaque.frag` declares**: ten fragment
		// samplers and three uniform buffers, in those slots. A shader object
		// carries those counts rather than the pipeline doing so, so a module
		// that declares different ones binds and silently samples nothing - the
		// same trap the built-in pipeline's own comments record. That interface
		// is what a `ShaderScript` is written against.
		//
		// **Only the fragment stage, deliberately.** The vertex stage owns the
		// instance layout, which `AGENTS.md` says is private and stays private -
		// a caller able to replace it would be authoring against a struct nobody
		// promised to keep.
		//
		// **Two pipelines are built, opaque and transparent**, because blend
		// state is baked into a pipeline. The shadow pass draws through neither:
		// it writes depth and no colour, so a fragment shader there would cost a
		// pass over the scene to produce nothing.
		//
		// Registering a name twice within one owner replaces it, which is what an author editing
		// a shader does; the frame in flight is waited for before the old
		// pipeline is released.
		//
		// @param name  What a material names it.
		// @param spirv The module's words. Copied into device objects; not retained.
		// @param owner Exact residency namespace; scoped shaders never borrow shared variants.
		// @return `false` when the module or either pipeline could not be built.
		//         The error is logged with the shader's name.
		// @since v0.15
		// Identical owner, name and words reuse the pipeline without a frame wait or invalidation.
		// @param gbuffer The same surface for the G-buffer pass, or empty.
		// @param vertex  The surface's `Vertex` hook in each engine vertex stage,
		//                in `VertexStage` order, or empty for the engine's own.
		bool AddShader(
			const core::Name &name,
			std::span<const uint32_t> spirv,
			core::Name owner = {},
			std::span<const uint32_t> gbuffer = {},
			std::span<const std::vector<uint32_t>> vertex = {}
		);

		// Reflects and admits a material fragment against the renderer's named
		// sampler slots, then installs it through the normal replacement path.
		bool AddMaterialShader(
			const core::Name &name,
			std::span<const uint32_t> spirv,
			core::Name owner = {},
			std::span<const uint32_t> gbuffer = {},
			std::span<const std::vector<uint32_t>> vertex = {}
		);

		// View-driven mip residency (R06): see `TextureTable::SetStreaming`. Each
		// frame every view asks for the detail its drawn textures need from their
		// projected size, from the camera, mirror surfaces and portals.
		void SetTextureStreaming(size_t budget, size_t uploadPerFrame, uint32_t baseSide = 256);
		// Holds a texture at `level` for `frames` frames, ahead of a teleport.
		void PrewarmTexture(const core::Name &name, uint32_t level, uint32_t frames, core::Name owner = {});
		// A streamed texture's requested and resident levels.
		TextureTable::StreamState TextureStreaming(const core::Name &name, core::Name owner = {}) const;
		// Device bytes of every content texture.
		size_t TextureBytes() const;

		// Reference path tracing (R08): traces `samples` more paths per pixel of
		// `view` through the scene's real geometry (mirrors, the sun, its lights
		// and every surface's colour and emission) and returns the running
		// average as RGBA floats, rows top first. A different camera or set of
		// instances starts the average again. Up to `MESH_BUILD_BUDGET` new mesh
		// trees are built per call; instances whose mesh waits are left out.
		bool TraceReference(const View &view, uint32_t samples, std::vector<float> &rgba, uint32_t bounces = 3);
		// Paths per pixel in the running average.
		uint32_t ReferenceSamples() const;

		// Builds the UV-space draw of a `bake/` surface from its
		// `ShaderAssembly::Bake` words, for the `SurfaceBake`s a view carries.
		bool AddBakeShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner = {});

		// Forgets a registered shader and frees its pipelines.
		//
		// Instances still naming it fall back to the engine's own shader rather
		// than vanishing - `MeshTable::Resolve`'s rule, and its reason.
		//
		// @param name The name to drop.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `false` for a name this renderer does not hold.
		// @since v0.15
		bool DropShader(const core::Name &name, core::Name owner = {});

		// Whether a shader is registered under this name.
		//
		// @param name The name.
		// @param owner The exact content namespace, or empty for shared content.
		// @return `true` when a variant exists for it.
		// @since v0.15
		bool HasShader(const core::Name &name, core::Name owner = {}) const;

		// Registers a cooked shader asset for render graph nodes.
		//
		// A `raster` or `dispatch` node whose `shader` parameter names this
		// resolves here before the engine's staged modules, so a baked module
		// published as content can drive a graph pass. Every vertex, fragment
		// and compute variant is kept by stage; a node takes the variant of the
		// stage it records. Material shaders stay on `AddShader`, which has a
		// fixed fragment interface this store does not check.
		//
		// Replacing or dropping a name waits for the frame in flight and
		// releases cached graph pipelines, so the next frame rebuilds them.
		//
		// @param name    What a graph node's `shader` parameter names.
		// @param data    The decoded asset. Copied.
		// @param failure Why nothing was registered.
		// @return `false` when no variant has a stage and payload this
		//         renderer can use.
		bool AddGraphShader(const core::Name &name, const assets::ShaderData &data, std::string &failure);

		// Forgets a graph shader. Nodes naming it fall back to staged modules.
		//
		// @return `false` for a name this renderer does not hold.
		bool DropGraphShader(const core::Name &name);

		// Whether a graph shader is registered under this name.
		bool HasGraphShader(const core::Name &name) const;

		// Registers one `LensShader` fragment module. Lens programs have exactly
		// two sampler slots, HDR scene colour then linear depth, and one pushed
		// `LensPassUniforms` block. They cannot choose targets or graph order.
		//
		// @param name  What a `ShaderLens` names.
		// @param spirv The compiled fragment module.
		// @param owner The view's exact residency namespace, with no shared fallback.
		// @return `false` when the module cannot form the constrained pipeline.
		// Identical owner, name and words reuse the pipeline without a frame wait or invalidation.
		bool AddLensShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner = {});

		// Forgets a registered lens pipeline.
		bool DropLensShader(const core::Name &name, core::Name owner = {});

		// Whether a lens pipeline is registered under this name.
		bool HasLensShader(const core::Name &name, core::Name owner = {}) const;

		// Exact accepted device program; zero means no installed lens for this owner.
		assets::ContentHash LensShaderHash(const core::Name &name, core::Name owner = {}) const;

		// Retain immutable captured lens programs independently of world replica residency.
		// Zero means empty input or refusal. Matching live name/hash sets share a token.
		uint64_t RetainPortalLensPrograms(const PortalCaptureLenses &lenses);
		// Drops one reference to a retained captured lens-program group.
		void ReleasePortalLensPrograms(uint64_t token);

		// Keys authored shaders (materials, the post-process grade, lenses) by
		// each view's world name, and a copied row's by its source world,
		// instead of by the content owner. For a host that keeps meshes and
		// imported textures shared but prepares shaders per world, as Studio
		// does, so a world that names no shader cannot drop another's. The
		// client's content owner is its world name, so it needs no switch.
		void SetShaderOwnersFollowWorlds(bool follow);

		// Reconciles one owner's device programs against current accepted modules.
		// Material and lens demand are separate; a missing grade restores engine tonemap.
		// Returns whether visible resources changed. Unchanged refusals are not retried.
		bool PrepareShaders(
			const ShaderLibrary &library,
			std::span<const core::Name> materials,
			std::span<const core::Name> lenses,
			core::Name postProcess,
			core::Name owner = {},
			std::span<const core::Name> passes = {}
		);

		// Selects this shader in place of the engine tonemap for one content owner.
		// Views resolve their exact owner; a scoped miss never borrows shared state.
		//
		// **Written against `tonemap.frag`'s own contract, not
		// `opaque.frag`'s** - one sampler holding the lit, still-HDR scene,
		// no bound uniform buffer, one `vec4` out to whatever target this
		// pass is writing. `scene::PostProcessing`'s own header carries the
		// full argument for why this is the tonemap slot rather than a pass
		// appended after it.
		//
		// **Only the frame the world presents, never a portal pane's own
		// preview.** A pane redraws through the engine's plain ACES tonemap
		// regardless of what the main view is doing, so a custom grade on
		// the screen does not also recolour every mirror and portal in it -
		// see `render/AGENTS.md` on why the shadow and surface passes draw
		// the whole scene while the screen pass draws only what the eye
		// sees; this is the identical split one stage later.
		//
		// Each owner selects one pipeline. A successful replacement retires the
		// previous pipeline after the frame wait; a failed replacement preserves it.
		//
		// @param name The selected shader name, retained for selection queries.
		// @param spirv The compiled words.
		// @param owner Exact content namespace. Empty retains the shared namespace.
		// @return `false` on a device, translation or pipeline failure.
		// @since v0.18
		// Identical owner, name and words reuse the pipeline without a frame wait or invalidation.
		bool
		SetPostProcessShader(const core::Name &name, std::span<const uint32_t> spirv, core::Name owner = {});

		// Goes back to the engine's own tonemap.
		//
		// @since v0.18
		void ClearPostProcessShader(core::Name owner = {});

		// The name last handed to `SetPostProcessShader` and still active,
		// or an invalid name when the engine's own tonemap is drawing.
		//
		// @since v0.18
		core::Name PostProcessShaderName(core::Name owner = {}) const;

		// Waits for the display and claims this frame's image, before the caller
		// has read a single event.
		//
		// **Optional, and the reason to call it is latency rather than
		// throughput.** `Render` waits for the swapchain itself when this was not
		// called, so an existing loop is correct without it and measures the same.
		// What it cannot be is *responsive*: the wait is the better part of a
		// frame with vertical sync on, and a loop that pumps events and then
		// waits has already read its input by the time it starts waiting - so
		// every frame is drawn from input one frame old, no matter how fast the
		// code between them is. Calling this first moves the dead time to before
		// the input is read.
		//
		// The frame is held until the next `Render` consumes it. Calling this
		// twice in one frame is safe and acquires once; not calling `Render`
		// afterwards submits an empty frame at `Shutdown` rather than leaking it.
		//
		// **This is why the split exists at all rather than `Render` simply
		// waiting later**: a swapchain image cannot be acquired without a command
		// buffer, and the wait is the acquisition. There is no ordering inside
		// one call that puts it after the caller's input.
		//
		// @return `false` when there was nothing to acquire - minimised or
		//         mid-resize, which is not an error and not a reason to stop
		//         ticking. Headless always succeeds; it waits for nothing.
		bool WaitForFrame();

		// `WaitForFrame` without the wait: claims this frame's image only if the
		// swapchain has one free now. A miss returns `false` straight away, so a
		// loop can keep simulating and poll again rather than sleep in the
		// driver. Pair with `SetDecoupledDisplay`.
		//
		// @return Whether a frame was claimed. Headless always succeeds.
		bool PollForFrame();

		// Chooses the present mode for a polled loop: mailbox where the window
		// supports it, so the display shows only the newest frame and never
		// blocks a submit, and vertical sync otherwise. Disabling restores
		// vertical sync; a caller wanting immediate mode calls
		// `SetVerticalSync(false)` after. Queued like `SetVerticalSync`.
		//
		// @return False without a device.
		bool SetDecoupledDisplay(bool enabled);

		// Whether the requested present mode is mailbox, so a submitted frame
		// replaces any queued one and presenting never blocks.
		bool PresentsLatestFrame() const;

		// Reports whether the caller is the thread that owns this renderer.
		//
		// Recording is single-threaded; the owner is the thread that initialised it.
		//
		// @return `true` when the current thread may record and submit.
		// @threadsafe
		bool IsOnOwningThread() const;

		// "vulkan", "metal", "direct3d12". Shown in the F3 panel, because the
		// first question about a performance report is which backend produced
		// it.
		//
		// The returned view belongs to the renderer and is invalidated by Shutdown
		// or destruction. It is empty before successful initialisation.
		std::string_view BackendName() const;

		// Reports logical bytes held by tracked GPU resources on this device.
		//
		// The snapshot is cheap enough for the heap profiler's one-second sample,
		// but it takes the tracker's lock and is not a per-draw counter.
		GpuMemoryStatistics MemoryStatistics() const;

		// Reports resident rows and the latest staged delta for each mesh the
		// renderer has seen. The snapshot is for diagnostics, not a draw path.
		std::vector<AssetResidencyStatistics> AssetResidencies() const;

		// Appends the logical GPU section to an existing process heap report.
		//
		// This is separate from `core::HeapProfile::WriteReport` because the core
		// layer cannot name a client renderer. The report stays one artifact at
		// the program boundary where both sources are visible.
		bool AppendMemoryReport(const std::filesystem::path &path) const;

		// Off presents without waiting for vblank, which is what makes a frame
		// time measure the engine rather than the display. Returns false, and
		// stays as it was, when the backend has no unsynchronised mode.
		//
		// This is on the renderer rather than something the caller does to the
		// window because the swapchain belongs to the GPU device, and the
		// device is behind the pimpl.
		//
		// The request is queued because applying it recreates the swapchain.
		//
		// @param enabled True to wait for vertical blank; false to request immediate presentation.
		// @return True when the requested mode was supported and taken.
		bool SetVerticalSync(bool enabled);

		// Records a graph-owned frame into one command buffer. World-scoped nodes
		// are shared by views with the same world and pipeline. All but the last
		// processed view require an offscreen target. Borrowed view data is consumed
		// before this call returns.
		//
		// The old fixed-pass entry point is intentionally absent. Every host names
		// its world, graph and inputs through `View`, even when it draws one camera.
		//
		// @return Submitted draw counts and whether the frame was presented.
		// @since v0.17
		FrameResult Render(
			std::span<const View> views,
			OverlayImage &overlay,
			FrameOverlayHook *gameInterfaceHook = nullptr,
			bool present = true,
			FrameOverlayHook *hostOverlayHook = nullptr
		);

		// Increments when Render starts recording a graph frame. Hosts that issue
		// nested renders use it to invalidate frame-scoped source assumptions.
		uint64_t RenderGeneration() const;

		// The newest completed texture for that scene slot.
		//
		// **Slots exist because an editor has more than one viewport.** A game
		// draws one view of one world and only ever uses slot 0. A studio
		// showing a server's world beside a client's keeps a target per panel -
		// and they must be *separate* targets, because the panels are different
		// sizes and a single shared one would be reallocated twice a frame as
		// each panel asked for its own dimensions. That reallocation is
		// measurable: it is a colour and a depth texture destroyed and created
		// per frame.
		//
		// Three completed frames are retained on the device. Submission fences
		// publish a new one only at a CPU synchronization point, so a UI repaint
		// either gets a whole new frame or keeps the previous one. It never binds
		// the target the renderer is currently replacing. An interface layer hands
		// the pointer straight to whatever draws it; Dear ImGui's SDL_GPU backend
		// uses the same `SDL_GPUTexture *` as its `ImTextureID`.
		//
		// @param slot Which offscreen target to ask about. A program with one
		//             viewport uses 0 and never passes this.
		// @return The texture, or `nullptr` before the first frame completes.
		void *SceneTexture(size_t slot = 0) const;

		// The counters belonging to the completed image `SceneTexture` returns.
		// They publish at the same fence, so a retained image is never labelled
		// with the draw counts of a newer frame still running on the device.
		FrameResult SceneFrameResult(size_t slot = 0) const;

		// Checks whether a recorded frame's spatial depth is ready for a query.
		SpatialDepthFrameStatus SpatialDepthStatus(SpatialDepthFrameKey frame);
		// Returns projection and extent only while this frame remains retained.
		std::optional<SpatialDepthFrameInfo> SpatialDepthInfo(SpatialDepthFrameKey frame);
		// Queues one pixel of the matching retained frame, or returns zero on refusal.
		uint64_t QueueSpatialDepth(SpatialDepthFrameKey frame, uint32_t x, uint32_t y);
		// Takes a completed query; pending queries remain owned by the renderer.
		SpatialDepthReadbackResult TakeSpatialDepth(SpatialDepthFrameKey frame, uint64_t token);
		// Cancels one query. Its resources retire when its fence completes.
		bool CancelSpatialDepth(SpatialDepthFrameKey frame, uint64_t token);

		// A graph resource's current device image. Supports the default PBR
		// resources and `colour`; returns null when the selected slot has not run
		// that graph yet.
		void *ResourceTexture(core::Name resource, size_t slot = 0) const;

		// Requests a coarse retained copy of a graph image. The renderer refreshes
		// it after the graph has produced every stage, so an editor may display it
		// at a lower rate without the live target changing underneath the panel.
		// Spectrum reversal changes only this copy and maps each displayed colour
		// channel from n to 255 - n.
		//
		// @param pipeline        The installed graph that owns the image.
		// @param resource        The graph image to retain.
		// @param slot            The viewport that produced it.
		// @param reverseSpectrum Whether to reverse its displayed RGB spectrum.
		void RefreshResourcePreview(
			core::Name pipeline, core::Name resource, size_t slot = 0, bool reverseSpectrum = false
		);

		// The most recently refreshed retained copy, or null before its first frame.
		void *ResourcePreviewTexture(core::Name pipeline, core::Name resource, size_t slot = 0) const;

		// How wide that copy is against its height.
		//
		// **A preview keeps the resource's own shape**, capped at 256 on its
		// long side, so a full-screen target's preview is as wide as the screen
		// - and a caller that drew it into a square slot squashed it. The
		// render pipeline canvas is such a caller; it hands this to
		// `angview::View::NodeImages` as the picture's aspect.
		//
		// @param pipeline The installed graph that owns the image.
		// @param resource The graph image.
		// @param slot     The viewport that produced it.
		// @return Width over height, or 0 when there is no such preview.
		float ResourcePreviewAspect(core::Name pipeline, core::Name resource, size_t slot = 0) const;

		// Keeps a copy of what a scene slot currently holds, under a name.
		//
		// **A slot is scratch and this is how a picture outlives it.** There are
		// a handful of slots and they are drawn into on rotation, so whatever is
		// in one is gone within a few frames - which is why the studio's mesh
		// preview could only ever show the row under the cursor. A captured copy
		// is an ordinary entry in the texture table: `TextureHandle` returns it,
		// `DropTexture` releases it, and a list can draw a hundred of them.
		//
		// **The drawn rectangle, not the allocation.** A target is rounded up to
		// a block, so the copy is `SceneTextureExtent`'s rectangle and samples
		// whole - a consumer needs the handle and nothing else, which is the
		// coupling this exists to end.
		//
		// **Costs device memory that nothing reclaims on its own.** Each capture
		// is four bytes a texel against `TextureTable::MAXIMUM_BYTES`, so a
		// caller building them per row owes an eviction policy; the studio's
		// thumbnail cache is one.
		//
		// @param slot The slot to copy. Must have been drawn into.
		// @param name The name to publish the copy under. Replaces one already
		//             there, releasing it.
		// @return `false` for a slot never drawn into, an invalid name, or a
		//         texture table with no room.
		// @since v0.10
		bool CaptureSceneTexture(size_t slot, const core::Name &name);

		// Which corner of that slot's texture the world is in.
		//
		// **It describes the texture `SceneTexture` returns right now**, which
		// is the one an interface recording a bind right now will sample - so
		// the two are read together and stay a matched pair through a resize.
		// On the frame a target is reallocated the interface binds the outgoing
		// texture, and this reports the outgoing texture's rectangle with it.
		//
		// While a panel is merely being dragged the allocation does not change,
		// so this trails the rectangle the frame is about to draw by one frame's
		// worth of drag - under a pixel of scale, against a whole stale frame
		// stretched to the wrong shape before targets were allocated in blocks.
		// See `SceneExtent`.
		//
		// @param slot Which offscreen target to ask about.
		// @return The drawn fraction, or the whole texture when that slot has
		//         never been drawn into.
		SceneExtent SceneTextureExtent(size_t slot = 0) const;

		// The drawn fraction of `ResourceTexture`, including half-resolution
		// resources such as ambient occlusion.
		SceneExtent ResourceTextureExtent(core::Name resource, size_t slot = 0) const;

		// Writes the next offscreen frame's world to a file, once.
		//
		// **What makes an editor checkable without a screen.** Driving a window
		// and photographing a display is a test that depends on nobody else
		// using the machine and on the compositor cooperating; this reads the
		// texture the frame was actually drawn into. It costs a fence wait, so
		// it happens on the frames a caller asks for and no others.
		//
		// Only the world, and deliberately: the chrome is drawn onto the window
		// and the window is the swapchain, which SDL does not promise is
		// readable. What this answers is "did the scene render", which is the
		// question a renderer is asked.
		//
		// **Which viewport, because a host with two draws them in turn.** The
		// request is made after one panel's `Render` returns and consumed by the
		// *next* one, so a caller that asked while panel A was showing got
		// panel B's picture - and with a scene per panel that is a photograph of
		// a world nobody named. Naming the slot makes the request wait for that
		// panel's turn instead of taking whichever came next.
		//
		// @param path Where to write a BMP. Empty cancels a pending request.
		// @param slot Which viewport's scene to photograph, or `ANY_VIEWPORT`
		//             for whichever draws next - which is the old behaviour and
		//             is right for a host with one panel.
		void RequestSceneCapture(std::filesystem::path path, size_t slot = ANY_VIEWPORT);

		// Owning-thread API. Zero refuses a request without a device, a zero byte
		// budget, while recording a batch, or when all four slots are occupied. Pixels across all
		// slots are bounded to 64 MiB of pixel payload. Copied world and snapshot
		// identities are each capped at 4096 bytes. Tokens are process unique and never reused.
		// Requests complete during the next Render batch. Take transfers ownership
		// and releases its slot; Cancel releases pending or completed work. A transfer
		// already recording cannot be cancelled during a frame hook. Shutdown
		// invalidates every ticket and releases all retained storage.
		uint64_t RequestSceneCapture(const SceneCaptureRequest &request);
		// Takes a completed result on the owner thread and consumes its ticket.
		// Returns empty for pending, unknown or zero tokens.
		std::optional<SceneCaptureResult> TakeSceneCapture(uint64_t token);
		// Releases pending or completed capture storage on the owner thread.
		// Returns false for zero, unknown or already-recording transfer tickets.
		bool CancelSceneCapture(uint64_t token);

		// Writes the next complete host overlay to a file, once.
		//
		// The swapchain is write-only, so a requested frame records the host
		// overlay into a readable target and blits that target to the window. The
		// extra image and blit exist only on a requested frame.
		//
		// This is asynchronous with respect to the caller: `Render` consumes the
		// request and performs the GPU wait needed to write the BMP.
		//
		// @param path Where to write a BMP. Empty cancels a pending request.
		void RequestWindowCapture(std::filesystem::path path);

		// Whether either kind of file capture is waiting for a frame.
		//
		// @return `true` until the renderer has completed the requested readback.
		bool CapturePending() const;

		// Any viewport will do. See `RequestSceneCapture`.
		//
		// @since v0.15
		static constexpr size_t ANY_VIEWPORT = static_cast<size_t>(-1);

		// The device and swapchain format a `FrameOverlayHook` builds against.
		//
		// @return The handles, both empty before Initialise.
		BackendHandles Backend() const;
		// Supplies fixed world/view ownership; Runtime adds its VM and storage identities.
		script::ComputeBridgeFactory ComputeForWorld(std::string world, uint32_t view = UINT32_MAX);
		// Pumps copied compute work even when presentation is skipped or minimized.
		void PumpCompute();

	  private:
		bool AdoptResourceImagesInternal(
			std::span<const uint64_t> tokens,
			std::span<const PortalImageBinding> bindings,
			std::span<uint64_t> handles,
			bool replaceExisting
		);
		uint64_t ComposePortalBodyImageInternal(
			const PortalImageCapture &capture,
			const View &body,
			const PortalImageBinding &output,
			std::span<const scene::DrawInstance> apertureRows = {},
			std::span<const core::CFrame> apertureJoints = {},
			std::span<const PortalView> apertures = {},
			bool freshOutput = false
		);
		// Records one prepared view for the graph batch. Kept private so a host
		// cannot bypass graph-owned world grouping and frame dispatch.
		FrameResult RenderView(
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			std::span<const scene::DrawInstance> instances,
			OverlayImage &overlay,
			std::span<const SurfaceView> surfaces,
			FrameOverlayHook *gameInterfaceHook,
			FrameOverlayHook *hostOverlayHook,
			const SceneTarget *sceneTarget,
			size_t targetSlot,
			const View &source,
			std::span<const ParticleBatch> particles,
			std::span<const effects::RibbonVertex> ribbonVertices,
			std::span<const effects::RibbonRun> ribbonRuns,
			std::span<const SceneLight> lights,
			std::span<const scene::DrawInstance> foreign,
			std::span<const PortalView> portals,
			bool present,
			core::Name pipeline,
			uint64_t world
		);

		// Aborts when the caller is not the owning thread. See `IsOnOwningThread`.
		//
		// @param what The call being refused, for the message.
		void RequireOwningThread(const char *what) const;

		// Checks an explicitly installed graph name. This deliberately does not
		// resolve the unnamed fallback for an arbitrary missing name.
		bool HasPipelineRevision(core::Name name, uint64_t revision) const;

		// Rebuilds the unnamed fallback from the selected capability tier.
		bool InstallEngineDefault(const graph::PipelineDocument &document);

		struct Impl;
		std::unique_ptr<Impl> State;
		std::unique_ptr<DataFactoryHookBind> HookBind;

		struct InstalledNodeHandler {
			core::Name Kind;
			NodeHandler Handler;
			NodeHandlerLifecycle Lifecycle;
			bool Live = false;
		};
		std::vector<InstalledNodeHandler> CustomNodeHandlers;

		// The recording of one view, which is where the node families reach
		// this state from.
		//
		// **A friend rather than a public `Impl`, and rather than lifting the
		// state out of this class.** Splitting `RenderView` put each node
		// family in its own translation unit, and every one of them records
		// against the device objects `Impl` holds - so the type has to be
		// nameable from `src/nodes/`. Naming it through one class keeps the
		// definition private to `src/RendererState.hpp` and adds no type a
		// consumer of this header can do anything with. `src/ViewRecording.hpp`
		// carries the split's argument; `docs/retired/ARCH_REVIEW.md` C2 is the finding.
		friend class ViewRecording;
		friend class DataFactoryHookBind;
		friend class FrameBatch;

		// The thread that called `Initialise`, and the only one that may record.
		//
		// Not atomic, unlike `ecs::Store::Owner`, and the difference is the
		// point: a store's owner is written every tick by whichever worker
		// picked the world up, so that write races other threads' reads. This is
		// written by the constructor and again by `Initialise`, then only read -
		// so any thread that could observe a torn value is a thread already
		// violating the contract this exists to state.
		std::thread::id Owner;
	};
}
