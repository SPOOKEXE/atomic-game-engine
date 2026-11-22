// What one view works out before any pass records. `ViewRecordingPack.cpp`
// packs its instances and `ViewRecordingFinish.cpp` holds what it does once
// every pass has recorded.
//
// **`Begin` is the first sixteen hundred lines of what `Renderer::RenderView`
// used to be**, and it is separated from the node families because it is the
// half that decides where the world goes, what survives culling, which mirrors
// and holes are worth drawing, and how big every image is - while they are the
// half that records. `Finish` is the other end: the compiled graph runs, the
// capture is served, the host chrome goes on, and the buffers are submitted.
//
// The declarations in `Begin` write straight into the recording's own members
// rather than into locals that are copied out afterwards. A second copy is a
// second thing to forget, and the passes read these values a thousand lines
// away from where they were decided.

#include "capture/ViewRecording.hpp"

#include "DisplayColour.hpp"
#include "SurfaceScale.hpp"
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

	namespace {
		bool NeedsMeshResidencyFilter(std::span<const scene::DrawInstance> rows) {
			return std::any_of(rows.begin(), rows.end(), [](const scene::DrawInstance &row) {
				return row.Mesh.IsValid();
			});
		}

	}

	bool ViewRecording::UploadCloudDensity() {
		const auto &cloud = Request.Source->CloudDensity;
		scene::CloudDensityGpuNode empty{};
		empty.ChildrenLow.fill(UINT32_MAX);
		empty.ChildrenHigh.fill(UINT32_MAX);
		const std::span<const scene::CloudDensityGpuNode> nodes =
			cloud && cloud->IsValid() ? std::span(cloud->Nodes) : std::span(&empty, size_t{1});
		const size_t bytes = nodes.size_bytes();
		if (bytes > UINT32_MAX) return false;
		if (State->CloudDensityCapacity < bytes) {
			State->CloudDensityUploadedNodes.clear();
			if (State->CloudDensityBuffer) gpu::ReleaseBuffer(State->Device, State->CloudDensityBuffer);
			if (State->CloudDensityTransfer)
				gpu::ReleaseTransferBuffer(State->Device, State->CloudDensityTransfer);
			State->CloudDensityBuffer = nullptr;
			State->CloudDensityTransfer = nullptr;
			SDL_GPUBufferCreateInfo buffer{};
			buffer.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			buffer.size = static_cast<uint32_t>(bytes);
			State->CloudDensityBuffer = gpu::CreateBuffer(State->Device, &buffer);
			SDL_GPUTransferBufferCreateInfo transfer{};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = static_cast<uint32_t>(bytes);
			State->CloudDensityTransfer = gpu::CreateTransferBuffer(State->Device, &transfer);
			if (!State->CloudDensityBuffer || !State->CloudDensityTransfer) return false;
			State->CloudDensityCapacity = static_cast<uint32_t>(bytes);
		}
		// The fallback node is present in every ordinary frame. Reuse the resident
		// buffer when its packed contents are unchanged, including across views.
		if (State->CloudDensityUploadedNodes.size() == nodes.size() &&
			std::memcmp(State->CloudDensityUploadedNodes.data(), nodes.data(), bytes) == 0)
			return true;
		void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->CloudDensityTransfer, false);
		if (!mapped) return false;
		std::memcpy(mapped, nodes.data(), bytes);
		SDL_UnmapGPUTransferBuffer(State->Device, State->CloudDensityTransfer);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(Command);
		if (!copy) return false;
		const SDL_GPUTransferBufferLocation source{State->CloudDensityTransfer, 0};
		const SDL_GPUBufferRegion destination{State->CloudDensityBuffer, 0, static_cast<uint32_t>(bytes)};
		SDL_UploadToGPUBuffer(copy, &source, &destination, true);
		SDL_EndGPUCopyPass(copy);
		State->CloudDensityUploadedNodes.assign(nodes.begin(), nodes.end());
		Result.UploadedBytes += bytes;
		return true;
	}

	ViewStart ViewRecording::Begin(const ViewRequest &request) {
		ENGINE_PROFILE_CAT("ViewRecording::Begin", core::ProfileCategory::Render);

		Request = request;
		if (request.Source != nullptr && request.Source->SpatialSnapshotSequence != 0)
			Result.SpatialSnapshot = SpatialSnapshotStatus::Unavailable;
		SurfacePixelsUsed = 0;
		Instances = request.Instances;
		Foreign = request.Foreign;
		if (request.Source->DirectionalShadowBounds) {
			const auto &bounds = *request.Source->DirectionalShadowBounds;
			const auto finite = [](const core::Vector3 &point) {
				return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
			};
			if (!finite(bounds.Minimum) || !finite(bounds.Maximum) || bounds.Minimum.X > bounds.Maximum.X ||
				bounds.Minimum.Y > bounds.Maximum.Y || bounds.Minimum.Z > bounds.Maximum.Z ||
				!finite(bounds.Size()) || !finite(bounds.Centre()) ||
				!std::isfinite(bounds.Size().Magnitude())) {
				ENGINE_WARN("view {} has invalid directional shadow bounds", request.TargetSlot);
				return ViewStart::Abandoned;
			}
		}
		if (request.Source->Projection) {
			const glm::mat4 &projection = *request.Source->Projection;
			bool finite = true;
			for (int column = 0; column < 4; column++) {
				for (int row = 0; row < 4; row++) {
					finite = finite && std::isfinite(projection[column][row]);
				}
			}
			const float determinant = glm::determinant(projection);
			if (!finite || !std::isfinite(determinant) || determinant == 0.0f) {
				ENGINE_WARN("view {} has an invalid fitted projection", request.TargetSlot);
				return ViewStart::Abandoned;
			}
		}

		// **The names the body below works in, bound to what the recording
		// keeps.** Each one *is* the member rather than a copy of it, which is
		// what makes a value the passes read impossible to forget to publish:
		// there is nothing to publish. See `src/nodes/` for the other half of
		// the same arrangement.
		Impl *const State = this->State;
		FrameResult &result = Result;
		const core::CFrame &cameraFrame = Request.CameraFrame;
		const core::CFrame &behaviourFrame = Request.VisibilityCameraFrame;
		const scene::Camera &camera = Request.Camera;
		OverlayImage &overlay = *Request.Overlay;
		const std::span<const SurfaceView> surfaces = Request.Surfaces;
		FrameOverlayHook *const gameInterfaceHook = Request.GameInterfaceHook;
		FrameOverlayHook *const hostOverlayHook = Request.HostOverlayHook;
		const SceneTarget *const sceneTarget = Request.Target;
		const size_t targetSlot = Request.TargetSlot;
		const View &source = *Request.Source;
		State->ActiveContentOwner = source.ContentOwner;
		State->ActiveImageGraphWorld = source.WorldName;
		State->ActiveLocalImageGraphWorld =
			source.ImageGraphWorldName.IsValid() ? source.ImageGraphWorldName : source.WorldName;
		ContentSignature = scene::MixSignature(State->ResourceEpoch, source.ContentOwner.Id());
		ContentSignature =
			scene::MixSignature(ContentSignature, source.LensContentOwner.value_or(source.ContentOwner).Id());
		ContentSignature = scene::MixSignature(ContentSignature, source.LensPrograms);
		ContentSignature = scene::MixSignature(ContentSignature, source.LensTimeSeconds.has_value());
		if (source.LensTimeSeconds)
			ContentSignature =
				scene::MixSignature(ContentSignature, std::bit_cast<uint32_t>(*source.LensTimeSeconds));
		ContentSignature = scene::MixSignature(ContentSignature, source.DirectionalShadowBounds.has_value());
		if (source.DirectionalShadowBounds) {
			const auto &bounds = *source.DirectionalShadowBounds;
			for (float component :
				 {bounds.Minimum.X,
				  bounds.Minimum.Y,
				  bounds.Minimum.Z,
				  bounds.Maximum.X,
				  bounds.Maximum.Y,
				  bounds.Maximum.Z})
				ContentSignature = scene::MixSignature(ContentSignature, std::bit_cast<uint32_t>(component));
		}
		ContentSignature = scene::MixSignature(ContentSignature, source.World);
		ContentSignature = scene::MixSignature(ContentSignature, source.WorldName.Id());
		ContentSignature = scene::MixSignature(ContentSignature, source.ImageGraphWorldName.Id());
		// Replacing a graph releases its targets even when world inputs stay unchanged.
		if (const auto *installed = State->PipelineFor(Request.Pipeline))
			ContentSignature = scene::MixSignature(ContentSignature, installed->Revision);
		for (const WorldContentOwner &binding : source.ForeignContentOwners) {
			ContentSignature = scene::MixSignature(ContentSignature, binding.World.Id());
			ContentSignature = scene::MixSignature(ContentSignature, binding.Owner.Id());
		}
		if (State->SlotAt(targetSlot).ContentSignature != ContentSignature) {
			Request.Damage.Scene = true;
			Request.Damage.Objects = true;
			Request.Damage.Environment = true;
			Request.Damage.Particles = true;
			Request.Damage.Portals = true;
			State->SlotAt(targetSlot).InstanceSourcesReady = false;
		}

		if (State->HasShadowCaptureRequest(Request.Pipeline, Request.TargetSlot) ||
			Request.Source->ImportedDirectionalShadow != 0)
			Request.Damage.Scene = true;

		const std::span<const effects::RibbonVertex> ribbonVertices = Request.RibbonVertices;
		const std::span<const SceneLight> lights = Request.Lights;
		const std::span<const PortalView> portals = Request.Portals;
		const bool present = Request.Present;
		const core::Name pipeline = Request.Pipeline;

		auto &selectedPipeline = Pipeline;
		auto &instances = Instances;
		auto &foreign = Foreign;
		auto &command = Command;
		auto &swapchain = Swapchain;
		auto &width = Width;
		auto &height = Height;
		auto &offscreen = Offscreen;
		auto &sceneWidth = SceneWidth;
		auto &sceneHeight = SceneHeight;
		auto &targetWidth = TargetWidth;
		auto &targetHeight = TargetHeight;
		auto &nearestPane = NearestPane;
		auto &drawCamera = DrawCamera;
		auto &matrices = Matrices;
		auto &behaviourMatrices = BehaviourMatrices;
		auto &lightViewProjection = LightViewProjection;
		auto &sceneBounds = SceneBounds;
		auto &plan = Plan;
		auto &ownCount = OwnCount;
		auto &sceneCount = SceneCount;
		auto &sceneOpaque = SceneOpaque;
		auto &sceneTransparent = SceneTransparent;
		auto &sceneReflected = SceneReflected;
		auto &reflectedCasters = ReflectedCasters;
		auto &surfaceCasters = SurfaceCasters;
		auto &opaqueCount = OpaqueCount;
		auto &plainOpaque = PlainOpaque;
		auto &surfaceInCamera = SurfaceInCamera;
		auto &transparentCount = TransparentCount;
		auto &plainTransparent = PlainTransparent;
		auto &transparentSurfaces = TransparentSurfaces;
		auto &instanceCount = InstanceCount;
		auto &uploadCount = UploadCount;
		auto &cameraRuns = CameraRuns;
		auto &haveInstances = HaveInstances;
		auto &haveOverlay = HaveOverlay;
		auto &haveShadow = HaveShadow;
		auto &occlusionCulling = OcclusionCulling;
		auto &accepted = Accepted;
		auto &acceptedCount = AcceptedCount;
		auto &claimed = Claimed;
		auto &wantSurface = WantSurface;
		auto &surfaceSignature = SurfaceSignature;
		auto &refreshCount = RefreshCount;
		auto &sceneEye = SceneEye;
		auto &frameSeconds = FrameSeconds;
		auto &portalOf = PortalOf;
		auto &havePortals = HavePortals;
		auto &portalLevels = PortalLevels;
		auto &panes = Panes;
		auto &havePanes = HavePanes;
		auto &anyPane = AnyPane;
		auto &paneWidth = PaneWidth;
		auto &paneHeight = PaneHeight;
		auto &surfaceBounces = SurfaceBounces;
		auto &mirrorLevels = MirrorLevels;
		auto &mirrorHistory = MirrorHistory;
		auto &surfaceVisible = SurfaceVisible;
		auto &surfaceCoverage = SurfaceCoverage;
		auto &currentLighting = CurrentLighting;
		auto &lightUniforms = SceneLights;
		auto &lighting = Lighting;
		auto &frameUniforms = Frame;
		auto &uniforms = Uniforms;
		auto &pbrDimensions = PbrDimensions;
		auto &viewTarget = ViewTarget;
		auto &colourTarget = ColourTarget;
		auto &windowTarget = WindowTarget;
		auto &depthTarget = DepthTarget;
		auto &sceneViewport = SceneViewport;
		auto &sceneScissor = SceneScissor;
		auto &sampler = Sampler;
		auto &depthBindings = DepthBindings;
		auto &lightingBindings = LightingBindings;
		auto &particleCount = ParticleCount;
		auto &ribbonCount = RibbonCount;
		auto &uploadOverlay = UploadOverlay;
		auto &drawInterface = DrawInterface;
		auto &drawHostOverlay = DrawHostOverlay;
		auto &timingSlot = TimingSlot;
		auto &openedMark = OpenedMark;
		auto &timedCommand = TimedCommand;
		auto &openedWall = OpenedWall;
		auto &mainGpuWorkRecorded = MainGpuWorkRecorded;
		auto &dedicatedComputeSubmitted = DedicatedComputeSubmitted;
		auto &cpuNodeWall = CpuNodeWall;

		const auto graphNode = [this](core::Name kind) { return GraphNode(kind); };
		const auto graphEnabled = [this](core::Name kind) { return GraphEnabled(kind); };
		const auto endIncompleteView = [this] { EndIncompleteView(); };
		const auto closePass = [this] { ClosePass(); };

		// **Which target this frame draws into, read by `EnsureScene`.** Passed
		// through a member rather than an argument because `EnsureScene` is
		// called from two places and threading a slot through both would put
		// the same value in two signatures that must agree.
		State->ActiveSlot = targetSlot;

		if (!State->Device) {
			return ViewStart::Abandoned;
		}

		State->ActiveGraph = pipeline;
		selectedPipeline = State->PipelineFor(pipeline);
		if (selectedPipeline == nullptr) {
			ENGINE_ERROR("render graph '{}' is not installed", pipeline.Text());
			State->BatchFailed = State->BatchActive;
			return ViewStart::Abandoned;
		}

		// The combined color must use the same screen layer as the captured GUI.
		// An idle view keeps its previously published snapshot without repainting.
		if (source.SpatialSnapshotSequence != 0 && Request.Damage.SceneImage()) {
			Request.Damage.GameInterface = true;
			// A new snapshot must pair its color with the scene depth from the same
			// render. Reuse is safe only while both remain unchanged.
			if (source.SnapshotLayersKnown && source.CaptureSpatialDepth) Request.Damage.Scene = true;
		}

		// How close the nearest hole is, and the near plane that follows from it.
		//
		// **The near plane a portal needs is not the one a scene authors.** Walk
		// up to a doorway with a hole in it and the last hand's width of the
		// approach is the whole illusion: an authored near plane slices the pane
		// open there and the wall beside it disappears. `scene::PortalNearPlane`
		// trades depth precision for that, and only while a hole is close enough
		// to need it - CodeParade's `GH_CLAMP(NearestPortalDist() * 0.5f, ...)`,
		// which the demo applies to its one and only camera.
		//
		// **Measured from the behavior eye**, because portal captures use that
		// pose even while the visible raster remains a free inspection view.
		nearestPane = std::numeric_limits<float>::infinity();
		float visibleNearestPane = std::numeric_limits<float>::infinity();
		for (const PortalView &portal : portals) {
			nearestPane = std::min(
				nearestPane,
				scene::RectangleDistance(portal.Centre, portal.First, portal.Second, behaviourFrame.Position)
			);
			visibleNearestPane = std::min(
				visibleNearestPane,
				scene::RectangleDistance(portal.Centre, portal.First, portal.Second, cameraFrame.Position)
			);
		}

		// The screen and behavior eye can be in different rooms. Adapt their near
		// planes separately so the inspection raster and portal captures each fit
		// the panes they actually use.
		drawCamera = camera;
		if (!source.Projection) {
			drawCamera.NearPlane = scene::PortalNearPlane(camera.NearPlane, visibleNearestPane);
		}
		scene::Camera behaviourCamera = camera;
		if (!source.Projection) {
			behaviourCamera.NearPlane = scene::PortalNearPlane(camera.NearPlane, nearestPane);
		}

		// **Claimed here only if the caller did not claim it first.** `WaitForFrame`
		// is what a latency-sensitive loop calls before it reads its input; a
		// caller that does not is no worse off than before, because this is the
		// same acquisition at the same point in the frame. See `Impl::BeginFrame`.
		if (State->BatchActive) {
			command = State->BatchSubmit.Command;
			State->GpuQueues.BindView(&Command);
			if (State->BatchFinal) {
				swapchain = State->BatchSwapchain;
				width = State->BatchWidth;
				height = State->BatchHeight;
			}
		} else if (present) {
			ENGINE_PROFILE_CAT("begin frame", core::ProfileCategory::Render);
			if (!State->BeginFrame()) {
				return ViewStart::Abandoned;
			}
			State->TakeFrame(command, swapchain, width, height);
		} else {
			command = SDL_AcquireGPUCommandBuffer(State->Device);
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (texture-only): {}", SDL_GetError());
				return ViewStart::Abandoned;
			}
		}
		if (State->BatchFirst || !State->BatchActive) {
			State->FrameCounter++;
			State->PreviewSubmitted = false;
			State->PollPreview();
		}

		// **Headless has no swapchain, so its size comes from the target** -
		// nothing else has an opinion about it.
		if (swapchain == nullptr) {
			if (sceneTarget == nullptr || !sceneTarget->IsValid()) {
				// A headless renderer with nowhere to draw is a caller mistake
				// rather than a state to tolerate: every pass would run and its
				// result would be discarded.
				endIncompleteView();
				return ViewStart::Abandoned;
			}

			width = sceneTarget->Width;
			height = sceneTarget->Height;
		}

		// --- where the world goes -------------------------------------------
		//
		// Resolved once, here, and everything downstream reads `sceneWidth` and
		// `sceneHeight` rather than the swapchain's. That is the whole reason
		// this is a few lines in one place instead of a conditional at each use:
		// the cull frustum, the projection and the depth buffer all have to
		// agree about how big the image is, and they are decided hundreds of
		// lines apart.
		//
		// The ordinary path falls back to the window when a target cannot be allocated.
		// A caller asking for a texture and getting a frame
		// it did not expect can see that something is wrong; one that gets no
		// frame at all sees a frozen editor.
		{
			ENGINE_PROFILE_CAT("ensure scene target", core::ProfileCategory::Render);
			offscreen = sceneTarget != nullptr && sceneTarget->IsValid() &&
						State->EnsureScene(sceneTarget->Width, sceneTarget->Height);
		}
		// The acquired swapchain belongs to P, so W must never take that fallback.
		if (State->BatchPresentationCommand && !offscreen) {
			endIncompleteView();
			return ViewStart::Abandoned;
		}

		if (State->Headless() && !offscreen) {
			// The target could not be allocated. Headless has no window to fall
			// back to, so the frame ends here rather than drawing into nothing.
			endIncompleteView();
			return ViewStart::Abandoned;
		}

		if (!offscreen && State->SlotAt(targetSlot).Texture != nullptr) {
			// Nothing asked for a texture this frame, so last frame's is
			// released rather than kept against a caller who might come back. A
			// viewport panel that was closed should not go on costing its
			// pixels.
			State->EnsureScene(0, 0);
		}

		sceneWidth = offscreen ? sceneTarget->Width : width;
		sceneHeight = offscreen ? sceneTarget->Height : height;

		// **What the pass is drawing *onto*, which is not what it draws.** An
		// offscreen target is allocated in blocks, so the attachment is at least
		// as big as the world's rectangle and usually bigger; the world fills
		// the corner and the viewport below is what confines it there. See
		// `SCENE_TARGET_BLOCK`.
		targetWidth = offscreen ? State->SlotAt(targetSlot).Width : width;
		targetHeight = offscreen ? State->SlotAt(targetSlot).Height : height;

		{
			// Nothing at all on a steady window, and a texture allocation on the
			// frame after a resize. Worth telling apart from the pass that uses
			// it, because one is every frame and the other is one frame.
			//
			// **Sized to the attachment rather than to the world.** SDL wants a
			// depth target whose dimensions match the colour target it is bound
			// beside, and the colour target is the block-rounded allocation -
			// not the rectangle the world is drawn into. Sizing this to the
			// world instead is a validation failure on the frames where the two
			// differ, which is nearly all of them.
			//
			// **The slot's own depth when drawing offscreen.** Two viewports of
			// different sizes sharing one depth texture made every frame
			// reallocate it twice - see `SceneSlot::Depth`.
			ENGINE_PROFILE_CAT("ensure depth", core::ProfileCategory::Render);

			bool depthReady = false;
			if (offscreen) {
				Impl::SceneSlot &slot = State->SlotAt(targetSlot);
				depthReady = State->EnsureDepthIn(
					slot.Depth, slot.DepthWidth, slot.DepthHeight, targetWidth, targetHeight
				);
			} else {
				depthReady = State->EnsureDepth(targetWidth, targetHeight);
			}

			if (!depthReady) {
				endIncompleteView();
				return ViewStart::Abandoned;
			}
		}

		// --- what is ready to be drawn ---------------------------------------
		//
		// **An instance naming a mesh this table does not hold is not drawn at
		// all**, and the distinction from an instance naming *no* mesh is the
		// whole of it:
		//
		//   - no mesh named - an ordinary `Part` - draws the default cube, which
		//     is what a part is.
		//   - a mesh named and not resident - a `MeshPart` whose geometry has
		//     not arrived - draws nothing.
		//
		// Without the second, `MeshTable::Resolve` hands back the default and a
		// scene of mesh parts comes up as a field of cubes that turn into models
		// one by one as the content lands. That is worse than an empty space: an
		// empty space reads as "still loading" and a wrong cube reads as the
		// asset being broken.
		//
		// **Filtered once here rather than inside the cull and the scene gather
		// separately.** Both read this span, and a test written into each would
		// be two places to keep in step - the exact duplication that made the
		// mirror pass and the camera pass disagree about `Transparency` before
		// `OrderScene` was one function.
		//
		// A span with no named meshes cannot lose rows. Keep it borrowed instead
		// of copying every default Part into a per-view buffer. Named meshes still
		// take the residency path so a missing asset never becomes a default cube.
		{
			ENGINE_PROFILE_CAT("classify/filter mesh rows", core::ProfileCategory::Render);

			State->DrawableHidden.clear();
			if (NeedsMeshResidencyFilter(instances)) {
				scene::KeepLoaded(
					instances,
					[State, &source](const scene::DrawInstance &row) {
						return State->Meshes.Has(
							row.Mesh,
							State->ViewMeshOwner(
								row.Mesh,
								source.ContentOwnerOf(row.SourceWorld),
								row.SourceWorld.IsValid() ? row.SourceWorld : source.WorldName
							)
						);
					},
					State->Drawable,
					source.EyeHiddenRows,
					source.EyeHiddenRows.empty() ? nullptr : &State->DrawableHidden
				);
				instances = State->Drawable;
			} else {
				// No row was removed, so source indices remain the drawable indices.
				State->DrawableHidden.assign(source.EyeHiddenRows.begin(), source.EyeHiddenRows.end());
			}

			// **The other worlds pay the same toll.** A destination whose meshes
			// have not arrived here would otherwise come up as the field of
			// cubes described above - seen through a portal, which is the one
			// place a viewer cannot walk over and check.
			if (NeedsMeshResidencyFilter(foreign)) {
				scene::KeepLoaded(
					foreign,
					[State, &source](const scene::DrawInstance &row) {
						return State->Meshes.Has(
							row.Mesh,
							State->ViewMeshOwner(
								row.Mesh,
								source.ContentOwnerOf(row.SourceWorld),
								row.SourceWorld.IsValid() ? row.SourceWorld : source.WorldName
							)
						);
					},
					State->DrawableForeign
				);
				foreign = State->DrawableForeign;
			}
		}

		// --- uploads --------------------------------------------------------

		const auto totalCount = static_cast<uint32_t>(instances.size());
		haveInstances = false;
		haveOverlay = false;

		// **Culled, then ordered, then uploaded** - and the sequence is the
		// point. Culling first means the sort runs over what survives rather
		// than over the world, and the upload carries only what is drawn.
		//
		// Culling follows the behavior camera. The screen matrices remain attached
		// to `cameraFrame`, so inspection can move without changing what the world
		// submits, the selected LOD, or the portal captures.

		// **Every surface camera's view, resolved before any pass runs.** Each
		// is used twice: to render into its own texture now, and - one frame
		// later, as `PreviousViewProjection` - to project that texture back onto
		// whatever samples it, including another mirror.
		//
		// The accepted views are gathered here rather than filtered at each use,
		// so the two passes that draw mirrors iterate the same list and cannot
		// disagree about which indices are live. A duplicate index is the one
		// case worth refusing outright: two views writing one texture would race
		// for the pair and neither would be the frame the screen then samples.
		acceptedCount = 0;

		// **This viewport's surfaces, and not the renderer's.** A reflection is
		// of the viewer, so a world drawn from two panels wants two images per
		// pane. Resolved once here and read everywhere below, so no pass can
		// reach the wrong bank. See `Impl::SurfaceBanks`.
		Bank = &State->SurfacesAt(targetSlot);
		Impl::SurfaceBank &bank = *Bank;

		// **The holes, claimed before the surfaces are**, so that a slot named by
		// both goes to the recursive pass. That order is the answer rather than a
		// tie-break: a same-world portal handed to the surface path draws from the
		// wrong viewpoint the moment it is seen through another hole, which is the
		// whole reason this pass exists - whereas a portal drawn recursively and
		// *also* given a surface camera merely wastes a scene pass on a texture
		// nothing samples.
		havePortals = false;
		for (const PortalView &portal : portals) {
			if (portal.Index < 0 || static_cast<size_t>(portal.Index) >= scene::MAX_SURFACES) {
				ENGINE_WARN(
					"portal index {} is outside 0..{}, so it draws flat",
					portal.Index,
					scene::MAX_SURFACES - 1
				);
				continue;
			}

			const auto index = static_cast<size_t>(portal.Index);
			if (portalOf[index] != nullptr) {
				ENGINE_WARN("two portals claim index {}; the second is ignored", portal.Index);
				continue;
			}

			portalOf[index] = &portal;
			claimed[index] = true;
			havePortals = true;
		}

		// **Read once here, so the pass that fills the pool and the pass that
		// samples it cannot disagree about how many levels there are.** The top
		// level is `portalLevels - 1`, which is the index the transparent surface
		// composition reads.
		portalLevels = std::min(
			Request.Source != nullptr && Request.Source->SurfaceBudget ? Request.Source->SurfaceBudget->Depth
																	   : State->PortalDepth,
			MAX_PORTAL_DEPTH
		);

		const bool sharedCaptures = graphEnabled(core::Name("surface-capture"));
		for (const SurfaceView &view : surfaces) {
			if (view.Index < 0 || static_cast<size_t>(view.Index) >= scene::MAX_SURFACES) {
				ENGINE_WARN(
					"surface camera index {} is outside 0..{}, so it renders nothing",
					view.Index,
					scene::MAX_SURFACES - 1
				);
				continue;
			}

			const auto index = static_cast<size_t>(view.Index);
			if (portalOf[index] != nullptr) {
				ENGINE_WARN(
					"surface camera {} names a slot a portal already draws; the recursive pass keeps it",
					view.Index
				);
				continue;
			}
			if (claimed[index]) {
				ENGINE_WARN("two surface cameras claim index {}; the second is ignored", view.Index);
				continue;
			}

			claimed[index] = true;

			// **No aspect ratio, and its absence is load-bearing.** A surface
			// frustum is fitted to the pane's four corners, so the texture's
			// shape is already inside the extents that produced this
			// projection; widening by the aspect again here would apply it
			// twice and stretch every mirror in the scene.
			AcceptedView entry;
			entry.Index = index;
			entry.View = &view;
			entry.ViewProjection = scene::ResolveSurfaceCamera(view.Frame, view.Projection).ViewProjection;
			entry.Sampling = entry.ViewProjection * view.Mapping;
			entry.ImageOpacity = std::clamp(view.ImageOpacity, 0.0f, 1.0f);
			entry.Effect = view.Effect;

			accepted[acceptedCount++] = entry;
		}

		// **The panes, by slot, for the levels below the first.** A surface
		// camera arrives here already placed - from the eye - and that is the one
		// viewpoint a recursion cannot use: a pane appearing inside another pane's
		// picture is looked at from *that* pane's camera. `scene::ReflectCamera`
		// will place it for any viewer, and what it needs is the rectangle, which
		// is what `SurfaceView::PaneNormal` carries.
		//
		// **Zero normal means "do not descend", and it is the ordinary case for
		// two kinds of view.** A camera parented to the world has no face to
		// reflect through, and a cross-world pane's picture is another simulation
		// rather than this one's geometry. Both keep the single eye-derived image
		// they have always had.
		anyPane = false;

		// The authored texture size of each pane, which is what the levels below
		// the first are rendered at. See the allocation in the recursion for why
		// they do not take the top level's screen-coverage scaling.

		for (size_t index = 0; index < acceptedCount; index++) {
			const SurfaceView &view = *accepted[index].View;
			if (view.PaneNormal.Magnitude() <= 0.0f || view.InstanceCount > 0) {
				continue;
			}

			scene::SurfacePane &pane = panes[accepted[index].Index];
			pane.Centre = view.PaneCentre;
			pane.Normal = view.PaneNormal;
			pane.First = view.PaneFirst;
			pane.Second = view.PaneSecond;
			pane.Surface = static_cast<int16_t>(accepted[index].Index);
			pane.TagFilter = view.TagFilter;
			pane.NearPlane = view.PaneNear;
			pane.FarPlane = view.PaneFar;

			paneWidth[accepted[index].Index] = view.Width;
			paneHeight[accepted[index].Index] = view.Height;

			havePanes[accepted[index].Index] = true;
			anyPane = true;
		}

		const float cameraAspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
		matrices = source.Projection ? scene::ResolveSurfaceCamera(cameraFrame, *source.Projection)
									 : scene::ResolveCamera(cameraFrame, drawCamera, cameraAspect);
		behaviourMatrices = behaviourFrame.FuzzyEq(cameraFrame, 0.0f) ? matrices
							: source.Projection
								? scene::ResolveSurfaceCamera(behaviourFrame, *source.Projection)
								: scene::ResolveCamera(behaviourFrame, behaviourCamera, cameraAspect);
		State->ActiveDataCaptureSource.ProjectionAvailable = true;
		State->ActiveDataCaptureSource.Width = sceneWidth;
		State->ActiveDataCaptureSource.Height = sceneHeight;
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				State->ActiveDataCaptureSource.Projection[column * 4 + row] =
					matrices.Projection[column][row];
		if (State->DataCaptureSources.size() <= targetSlot) {
			State->DataCaptureSources.resize(targetSlot + 1);
		}
		State->DataCaptureSources[targetSlot] = State->ActiveDataCaptureSource;
		if (sharedCaptures) {
			const auto budget = source.SurfaceBudget.value_or(
				View::SurfaceCaptureBudget{
					std::max(portalLevels, std::max(1u, State->SurfaceBounces)), UINT64_MAX
				}
			);
			portalLevels = budget.Depth;
			uint64_t lightPixels = 0;
			for (const auto &portal : portals) {
				if (!portal.ExternalImage)
					lightPixels += uint64_t(SEAM_LIGHT_RESOLUTION) * SEAM_LIGHT_RESOLUTION;
			}
			const uint64_t capturePixels = lightPixels <= budget.Pixels ? budget.Pixels - lightPixels : 0;
			const SurfaceCaptureRequest keyedCaptureRequest{
				.Mirrors = surfaces,
				.Portals = portals,
				.Frame = behaviourFrame,
				.Projection = BehaviourMatrices.Projection,
				.PixelBudget = capturePixels,
				.Width = sceneWidth,
				.Height = sceneHeight,
				.Depth = budget.Depth,
				// An explicit capture budget owns the whole path. Otherwise the
				// authored mirror count must not inherit the portal depth default.
				.MirrorDepth = source.SurfaceBudget || State->SurfaceBounces == 0 ? 32 : State->SurfaceBounces
			};
			const uint64_t captureSignature = SurfaceCaptureSignature(keyedCaptureRequest);
			if (bank.CaptureCache.NeedsRefresh(captureSignature)) {
				result.SurfaceCapturePlanWrite = true;
				SurfaceCaptureStatus captureStatus;
				{
					ENGINE_PROFILE_CAT("plan surface captures", core::ProfileCategory::Render);
					captureStatus = PlanSurfaceCaptures(keyedCaptureRequest, bank.CapturePlan);
				}
				if (captureStatus == SurfaceCaptureStatus::Invalid) {
					EndIncompleteView();
					return ViewStart::Abandoned;
				}
				result.SurfaceBudgetExceeded = lightPixels > budget.Pixels ||
											   (lightPixels > 0 && budget.Depth == 0) ||
											   captureStatus == SurfaceCaptureStatus::BudgetExceeded;
				if (result.SurfaceBudgetExceeded) {
					bank.CapturePlan.Entries.clear();
					bank.CapturePlan.Postorder.clear();
					bank.CapturePlan.Roots.fill(NO_SURFACE_CAPTURE);
				}
				bank.CaptureCache.Commit(
					captureSignature,
					result.SurfaceBudgetExceeded ? 0 : bank.CapturePlan.Pixels + lightPixels,
					result.SurfaceBudgetExceeded
				);
			} else {
				result.SurfaceBudgetExceeded = bank.CaptureCache.BudgetExceeded;
			}
			if (!result.SurfaceBudgetExceeded) {
				SurfacePixelsUsed = bank.CaptureCache.Pixels;
			} else {
				SurfacePixelsUsed = 0;
			}
		}

		bool sceneBoundsReady = false;

		graph::EntityFlow &entityFlow = State->GraphEntities;
		graph::Viewpoints &viewpoints = State->GraphViewpoints;
		entityFlow.Clear();
		viewpoints.Clear();
		graph::Viewpoint fallbackViewpoint;
		fallbackViewpoint.Frame = behaviourFrame;
		fallbackViewpoint.Lens = behaviourCamera;
		fallbackViewpoint.Projection = behaviourMatrices.ViewProjection;
		fallbackViewpoint.Fitted = true;

		size_t visibleCount = instances.size();
		opaqueCount = 0;
		core::Name orderedEntities;
		const bool profileCpuNodes = State->ProfileTier != ProfilingTier::Off;
		{
			ENGINE_PROFILE_CAT("observe scene visibility", core::ProfileCategory::Render);
			State->VisibilityWorking.Begin(
				State->FrameCounter, Request.TargetSlot, Request.Source->WorldName
			);
			for (const scene::DrawInstance &instance : instances) {
				State->VisibilityWorking.Observe(instance);
			}
		}

		// **The whole CPU half of the pipeline, and it had no span.** Frustum
		// culling, distance culling, tag filtering and the draw-order sort all
		// run here, before a single GPU call, and every one of them walks the
		// draw list. `cpuNodeWall` beside it has been measuring them for the
		// pipeline panel the whole time; the frame graph could not see them at
		// all, so they read as a blank at the top of `Renderer::RenderView`.
		{
			ENGINE_PROFILE_CAT("entity nodes", core::ProfileCategory::Render);
			for (const graph::NodeId id : selectedPipeline->EntityNodes) {
				const graph::Node *node = selectedPipeline->Graph.Find(id);
				if (node == nullptr) {
					continue;
				}
				std::span<const uint32_t> frustumInput;
				if (node->Kind == core::Name("cull-frustum")) {
					for (const graph::ResourceId resource : node->Reads) {
						const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResourceDesc(resource);
						if (desc != nullptr && desc->Kind == graph::ResourceKind::Entities) {
							frustumInput = entityFlow.Get(desc->Name);
							break;
						}
					}
				}
				const auto started = profileCpuNodes ? std::chrono::steady_clock::now()
													 : std::chrono::steady_clock::time_point{};
				const graph::EntityNodeRun run = graph::RunEntityNode(
					selectedPipeline->Graph,
					*node,
					instances,
					fallbackViewpoint,
					cameraAspect,
					entityFlow,
					viewpoints
				);
				if (!run.Handled) {
					continue;
				}
				if (!frustumInput.empty() && run.Output.IsValid()) {
					const std::span<const uint32_t> survivors = entityFlow.Get(run.Output);
					size_t survivor = 0;
					for (const uint32_t candidate : frustumInput) {
						if (survivor < survivors.size() && survivors[survivor] == candidate) {
							survivor++;
							continue;
						}
						if (candidate < instances.size()) {
							State->VisibilityWorking.FrustumCulled(instances[candidate]);
						}
					}
				}
				if (run.BoundedAll) {
					sceneBounds = run.Bounds;
					sceneBoundsReady = true;
				}
				visibleCount = run.Output.IsValid() ? run.Count : visibleCount;
				if (run.Ordered) {
					opaqueCount = run.Opaque;
					orderedEntities = run.Output;
				}
				if (profileCpuNodes) {
					cpuNodeWall[node->Name.Id()] +=
						std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
							.count();
				}
			}
		}
		if (!sceneBoundsReady) {
			sceneBounds = graph::BoundsOfAll(instances);
		}

		const std::span<const uint32_t> ordered = entityFlow.Get(orderedEntities);
		{
			// The caller or Drawable retains the source; copy only its camera order.
			ENGINE_PROFILE_CAT("copy draw lists", core::ProfileCategory::Render);
			State->VisibleInstances = instances;
			State->DrawOrder.assign(ordered.begin(), ordered.end());
		}
		const auto isEyeBody = [&source](const scene::DrawInstance &row) {
			const bool ownWorld = !row.SourceWorld.IsValid() || row.SourceWorld == source.WorldName;
			return source.EyeRig != 0 && ownWorld && row.Variant == 0 &&
				   (row.Rig == source.EyeRig || row.Source == source.EyeRig);
		};
		if (source.EyeRig != 0 || !source.EyeHiddenRows.empty()) {
			ENGINE_PROFILE_CAT("eye body selection", core::ProfileCategory::Render);
			size_t kept = 0, opaqueKept = 0;
			for (size_t index = 0; index < State->DrawOrder.size(); ++index) {
				const uint32_t rowIndex = State->DrawOrder[index];
				if (rowIndex >= instances.size()) continue;
				const auto &row = instances[rowIndex];
				if (std::binary_search(State->DrawableHidden.begin(), State->DrawableHidden.end(), rowIndex))
					continue;
				if (isEyeBody(row)) continue;
				State->DrawOrder[kept++] = rowIndex;
				opaqueKept += index < opaqueCount;
			}
			State->DrawOrder.resize(kept);
			opaqueCount = opaqueKept;
		}
		visibleCount = State->DrawOrder.size();

		DirectionalShadowBounds = source.DirectionalShadowBounds.value_or(sceneBounds);
		if (!source.DirectionalShadowBounds && (source.EyeRig != 0 || !source.EyeHiddenRows.empty())) {
			// The first-person body does not draw or cast in its own view. Keeping
			// it in the fitted domain still moves the shadow raster with the eye,
			// which is visible as fast crawling in an otherwise static world.
			bool haveCaster = false;
			for (size_t index = 0; index < instances.size(); ++index) {
				if (std::binary_search(
						State->DrawableHidden.begin(),
						State->DrawableHidden.end(),
						static_cast<uint32_t>(index)
					) ||
					isEyeBody(instances[index]))
					continue;
				const core::AABB bounds = graph::BoundsOf(instances[index]);
				DirectionalShadowBounds = haveCaster ? DirectionalShadowBounds.Union(bounds) : bounds;
				haveCaster = true;
			}
		}
		if (source.DirectionalShadowBounds && !instances.empty() &&
			(!DirectionalShadowBounds.Contains(sceneBounds.Minimum) ||
			 !DirectionalShadowBounds.Contains(sceneBounds.Maximum))) {
			ENGINE_WARN("view {} has directional shadow bounds smaller than its scene", request.TargetSlot);
			endIncompleteView();
			return ViewStart::Abandoned;
		}

		// **Fitted to the whole draw list, not to what survived culling.** A
		// caster outside the camera's frustum still shadows into it, so the
		// light has to see everything - and fitting to the culled set is the
		// classic version of this bug: shadows that vanish as their casters
		// leave the screen. That is why `sceneBounds` is the union over
		// `instances` and not over `State->Visible`.
		//
		// The graph cull only changes what the view draws. Shadows still fit the
		// whole world so an off-screen caster cannot disappear from the map.
		// Split source/body views supply the same enclosing domain for the same raster grid.
		lightViewProjection = graph::FitDirectionalLight(
			DirectionalShadowBounds,
			core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
			SHADOW_RESOLUTION
		);
		if (source.DirectionalShadowBounds) {
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					if (!std::isfinite(lightViewProjection[column][row])) {
						ENGINE_WARN("view {} has an invalid directional shadow fit", request.TargetSlot);
						endIncompleteView();
						return ViewStart::Abandoned;
					}
		}
		transparentCount = static_cast<uint32_t>(visibleCount - opaqueCount);
		if (source.ImportedDirectionalShadow &&
			(!graphEnabled(core::Name("shadow")) ||
			 !State->ValidPortalShadowView(source, lightViewProjection))) {
			ENGINE_WARN("view {} has a mismatched imported directional shadow", request.TargetSlot);
			endIncompleteView();
			return ViewStart::Abandoned;
		}

		// **Surface instances moved to the back of the opaque head**, so the
		// camera range is three contiguous runs - plain opaque, then mirrors,
		// then transparent - and each is one draw with one `first_instance`.
		// Whether an instance samples the surface is per instance and the
		// uniform that says so is per draw, so the alternative is a branch on
		// data the fragment shader does not have.
		//
		// Stable, for the reason the ordering itself is: an opaque scene with no
		// mirrors must come out of this exactly as it went in.
		surfaceInCamera = 0;
		if (opaqueCount > 0) {
			ENGINE_PROFILE_CAT("partition surfaces", core::ProfileCategory::Render);

			// **`scene::PartitionSurfaces`, not a fourth copy of it.** The
			// comment forty lines down insists the mirror partition lives in
			// `scene` "where a headless suite can get at them" - and this file
			// had two hand-rolled copies of it, which is what that sentence
			// exists to prevent. They are one function now, and it is the tested
			// one.
			surfaceInCamera = static_cast<uint32_t>(scene::PartitionSurfaces(
				State->VisibleInstances, std::span<uint32_t>(State->DrawOrder.data(), opaqueCount)
			));
		}
		plainOpaque = static_cast<uint32_t>(opaqueCount) - surfaceInCamera;

		// **Grouped by index within that run, because each index owns a
		// texture.** The screen pass binds a sampler and pushes a projection per
		// surface, so what used to be one draw over "the mirrors" is one draw
		// per surface - and each has to be contiguous for that to be an offset
		// and a count rather than a per-instance branch.
		//
		// `scene::GroupSurfaces`, not a second copy of it: the scene range is
		// grouped by `OrderScene` using the same function, and two groupings
		// that disagreed would put a pane's reflection on another pane's pass.
		if (surfaceInCamera > 0) {
			ENGINE_PROFILE_CAT("group surfaces", core::ProfileCategory::Render);
			scene::GroupSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(State->DrawOrder.data() + plainOpaque, surfaceInCamera),
				plainOpaque,
				true,
				cameraRuns
			);
		}

		// **And the same split at the end of the blended tail, which is what
		// makes a faded mirror still a mirror.** A part leaves the opaque head
		// the moment its `Transparency` goes above zero - and the head is where
		// the mirror flag was set, so the reflection did not dim, it vanished.
		// That reads as the surface camera having stopped rather than as an
		// ordering rule, and it is the bug this run exists to fix.
		//
		// They go *last* of everything, so they draw over the blended geometry
		// as well as the opaque. Stable, so the back-to-front sort survives
		// inside each run - see `scene::ScenePlan::TransparentSurfaces` for what
		// is given up across the two.
		transparentSurfaces = 0;
		if (transparentCount > 0) {
			ENGINE_PROFILE_CAT("partition blended surfaces", core::ProfileCategory::Render);

			transparentSurfaces = static_cast<uint32_t>(scene::PartitionSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(State->DrawOrder.data() + opaqueCount, transparentCount)
			));
		}
		plainTransparent = transparentCount - transparentSurfaces;

		if (transparentSurfaces > 0) {
			ENGINE_PROFILE_CAT("group blended surfaces", core::ProfileCategory::Render);
			scene::GroupSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(
					State->DrawOrder.data() + opaqueCount + plainTransparent, transparentSurfaces
				),
				static_cast<uint32_t>(opaqueCount) + plainTransparent,
				false,
				cameraRuns
			);
		}

		// The flip from transparency to opacity happened where each view was
		// accepted, once per surface, rather than in a shader nobody can put a
		// breakpoint in. `Impl::SurfaceSlotState::ImageOpacity` holds it.

		// **A second range holding everything, for the two passes that are not
		// the camera's.** A caster outside the camera's frustum still shadows
		// into it, and a mirror shows what is behind the viewer - so culling to
		// the eye would give shadows that vanish as their casters leave the
		// screen and a mirror that reflects only what is already on screen.
		// Both are the classic version of this mistake.
		//
		// Ordered from the surface camera when there is one, because the surface
		// pass is the only consumer that needs an order at all - a depth-only
		// pass does not care.
		// **Allocated for every accepted view before anything is ordered**, so
		// a view whose texture cannot be made drops out of the frame here rather
		// than half way through the pass loop.
		// **Whether anything can see each pane, which is the other half of the
		// refresh decision and the half this pass did not have.** The signature
		// answers "did the image change"; nothing answered "is the image
		// looked at", so a room of mirrors redrew every one of them on every
		// frame anything moved - including the ones behind the viewer and the
		// ones a wall stands in front of. That cost is per pane and the scenes
		// this exists for are the ones with several.
		//
		// **Two sweeps and deliberately not a fixed point.** A pane visible only
		// *inside another mirror* is a real case - two facing panes, or a portal
		// seen through a portal - so main-camera visibility alone would freeze
		// it. The union with the other surfaces' own frusta covers it in one
		// pass: a pane seen inside a surface that is itself on screen refreshes
		// now, and one buried two bounces deep refreshes a frame later. That is
		// the same one-frame budget the whole surface pass already runs on, and
		// iterating to closure here would spend the saving this exists for.
		// **How deep the mirrors go this frame, decided once and read twice.**
		// The cull below marks that many levels visible and the pass below draws
		// that many; a level drawn and not marked is a level culled, which is the
		// defect the two expressions were allowed to drift into once already. One
		// name is what stops it happening again.
		//
		// **A stated number wins outright and the measurement is the default.** A
		// world's `workspace.SurfaceBounces` and `--surface-bounces` both arrive
		// through `SetSurfaceBounces`, so by here the choice is only whether one
		// was made. Nothing is stated: `SurfaceBank::Bounces` is what the frame
		// before this one reached, and `scene::NextSurfaceBounces` turns it into
		// what to draw now.
		//
		// **And a frame with no pane rectangle keeps the old constant**, because
		// the iterating path is not the thing being measured - a cross-world pane
		// shows a second simulation and a camera parented to the world has no
		// face, so neither can be descended into and neither reports a depth.
		const graph::Node *mirrorCapture = graphNode(core::Name("mirror-capture"));
		const graph::Node *captureFormatNode =
			mirrorCapture ? mirrorCapture : graphNode(core::Name("surface-capture"));
		MirrorFormat = State->ColourFormat();
		if (captureFormatNode != nullptr) {
			for (const auto resource : captureFormatNode->Writes) {
				const auto *desc = selectedPipeline->Graph.FindResourceDesc(resource);
				if (desc != nullptr && desc->Format == graph::ResourceFormat::RGBA16F) {
					MirrorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
					break;
				}
			}
		}
		const uint32_t mirrorDepthLimit = std::clamp(
			mirrorCapture != nullptr ? mirrorCapture->Integer(core::Name("max-recursion"), MAX_SURFACE_DEPTH)
									 : MAX_SURFACE_DEPTH,
			1u,
			MAX_SURFACE_DEPTH
		);
		mirrorHistory = mirrorCapture == nullptr ||
						mirrorCapture->Parameter(core::Name("feedback")) == nullptr ||
						*mirrorCapture->Parameter(core::Name("feedback")) != "flat";
		surfaceBounces = State->SurfaceBounces > 0
							 ? std::min(State->SurfaceBounces, mirrorDepthLimit)
							 : (anyPane ? scene::NextSurfaceBounces(bank.Bounces, mirrorDepthLimit)
										: std::min(scene::DEFAULT_SURFACE_BOUNCES, mirrorDepthLimit));

		if (Request.Source != nullptr && Request.Source->SurfaceBudget) {
			surfaceBounces = std::min(surfaceBounces, std::max(1u, Request.Source->SurfaceBudget->Depth));
		}

		// What this frame reaches, which the next one reads back out of the bank.
		//
		// **Rebuilt every frame rather than accumulated.** A viewer who turns
		// away from a corridor has to come back down as fast as they went up, and
		// a running maximum would hold the deepest thing anybody ever saw for the
		// rest of the session.

		if (acceptedCount > 0) {
			graph::SurfaceEye eyes[scene::MAX_SURFACES];
			for (size_t index = 0; index < acceptedCount; index++) {
				eyes[index].ViewProjection = accepted[index].ViewProjection;
				eyes[index].Index = static_cast<int16_t>(accepted[index].Index);
			}

			// **In `graph` rather than here, and that is the seam rather than
			// tidiness.** The decision is boxes against frusta over a draw list,
			// which is what that module is, and a rule this pass held privately
			// would be a rule no suite could reach - `Renderer::Render` needs a
			// device and the answer does not.
			// **`surfaceBounces` and not a constant.** The pass below resolves
			// that many levels of surface-seen-in-surface, so a level this marks
			// invisible is a level that is *culled* rather than one frame late.
			// The two numbers were allowed to disagree when `D00112` made the
			// pass recursive, and the result was a mirror's deeper reflections
			// vanishing as the viewer turned - the moment a pane left the
			// frustum, everything it had been revealing dropped past the single
			// level this used to follow.
			//
			// **One name rather than the same expression written twice**, which
			// is what that drift cost and is why the depth is decided above
			// this block instead of inside it.
			const uint32_t cullRounds = acceptedCount > 1 ? std::max(surfaceBounces, 1u) : 1u;

			(void)graph::VisibleSurfaces(
				instances,
				matrices.ViewProjection,
				std::span<const graph::SurfaceEye>(eyes, acceptedCount),
				std::span<bool>(surfaceVisible, scene::MAX_SURFACES),
				std::span<float>(surfaceCoverage, scene::MAX_SURFACES),
				cullRounds
			);
		}

		// **The world's pane budget, spent on the panes covering the most
		// screen.** A pane is a whole render of the world into a texture, so the
		// count is what a hall of mirrors costs after the depth - see
		// `Renderer::SetSurfaceLimit`, and `scene::SurfaceLimit` for why the
		// number belongs to the world rather than to the session.
		//
		// **Ranked and then compacted *in place*, so the survivors keep their
		// original order.** Sorting `accepted` outright would be simpler and
		// would also move whichever view lands at slot zero, which is the one
		// the blended tail is sorted for a few lines below - so a mirror
		// becoming slightly more visible would silently re-sort every
		// transparent thing in the frame.
		//
		// **A view that loses its budget is not cleared.** It keeps whatever
		// texture it last drew, exactly as one skipped for its refresh rate
		// does, so a mirror the player has turned away from goes stale rather
		// than blank - which is the failure nobody notices, and the point of
		// ranking by coverage in the first place.
		if (!sharedCaptures && acceptedCount > State->SurfaceLimit) {
			size_t ranked[scene::MAX_SURFACES];
			for (size_t index = 0; index < acceptedCount; index++) {
				ranked[index] = index;
			}

			// Largest coverage first, and the slot number breaks a tie - so two
			// panes a frustum measured identically are dropped in a stated
			// order rather than in whichever order the sort happened to leave
			// them, which is what would make a frame differ from itself.
			std::stable_sort(
				ranked, ranked + acceptedCount, [&accepted, &surfaceCoverage](size_t left, size_t right) {
					const float first = surfaceCoverage[accepted[left].Index];
					const float second = surfaceCoverage[accepted[right].Index];
					if (first != second) {
						return first > second;
					}
					return accepted[left].Index < accepted[right].Index;
				}
			);

			bool afford[scene::MAX_SURFACES] = {};
			for (size_t rank = 0; rank < State->SurfaceLimit; rank++) {
				afford[ranked[rank]] = true;
			}

			size_t kept = 0;
			for (size_t index = 0; index < acceptedCount; index++) {
				if (afford[index]) {
					accepted[kept++] = accepted[index];
				}
			}
			acceptedCount = kept;
		}

		// **Where surface textures are created and released**, which is device
		// work in the middle of a frame and had nothing naming it. It reads as
		// zero until a pane changes how much of the screen it covers, and as a
		// spike on the frame it does - which is exactly the reading somebody
		// chasing a hitch while walking towards a mirror needs.
		size_t liveCount = 0;
		{
			ENGINE_PROFILE_CAT("ensure surfaces", core::ProfileCategory::Render);

			for (size_t index = 0; index < acceptedCount; index++) {
				const AcceptedView &view = accepted[index];
				if (sharedCaptures) {
					const uint16_t root = bank.CapturePlan.Roots[view.Index];
					if (root != NO_SURFACE_CAPTURE) {
						const auto &capture = bank.CapturePlan.Entries[root];
						if (State->EnsureSurface(
								targetSlot, view.Index, capture.Width, capture.Height, MirrorFormat
							)) {
							accepted[liveCount++] = view;
						} else {
							EndIncompleteView();
							return ViewStart::Abandoned;
						}
					}
					continue;
				}

				// **Sized to what the pane covers, not to what was authored.** The
				// authored size is a floor and the screen is a ceiling; between them
				// the target doubles as the pane grows on screen, which is what stops
				// a portal going coarse when you walk into it. See `SurfaceScale`.
				const uint32_t authored = std::max<uint32_t>(view.View->Width, view.View->Height);
				const Impl::SurfaceSlotState &sized = bank.Surfaces[view.Index];
				const uint32_t held = std::max<uint32_t>(sized.Width, sized.Height);
				const uint32_t current = authored > 0 && held >= authored ? held / authored : 1u;

				const uint32_t scale = SurfaceScale(
					authored,
					surfaceCoverage[view.Index],
					std::max<uint32_t>(sceneWidth, sceneHeight),
					current
				);

				if (Request.Source != nullptr && Request.Source->SurfaceBudget) {
					if (!surfaceVisible[view.Index]) {
						continue;
					}
					// A failed resize may retain the old pair. Reserve its extent too,
					// so keeping it cannot silently exceed the capture budget.
					const uint32_t budgetWidth = std::max(view.View->Width * scale, sized.Width);
					const uint32_t budgetHeight = std::max(view.View->Height * scale, sized.Height);
					if (!AdmitSurfaceCapture(budgetWidth, budgetHeight, 1)) {
						continue;
					}
				}

				if (State->EnsureSurface(
						targetSlot,
						view.Index,
						view.View->Width * scale,
						view.View->Height * scale,
						MirrorFormat
					)) {
					accepted[liveCount++] = view;
				}
			}
		}
		acceptedCount = liveCount;

		// **Ordered from the first surface camera when there is one.** The
		// blended sort is per view and there is only one scene range, so several
		// surfaces cannot each have the tail sorted for them - the first is the
		// one that gets it, and every other surface draws that order. Blended
		// geometry inside a reflection of a reflection is therefore sorted for
		// the wrong eye, which is a compositing error confined to the second
		// bounce and cheaper than an ordering pass per surface.
		wantSurface = acceptedCount > 0;
		sceneEye = wantSurface ? accepted[0].View->Frame.Position : behaviourFrame.Position;

		// **One signature shared by every surface, and that is not a shortcut.**
		// Each camera's matrix is in it because a surface pass draws the *other*
		// mirrors, projecting each one's texture with the camera that rendered
		// it - so a camera that moves changes how it appears inside every other
		// one. Every input to any surface is therefore an input to all of them,
		// and computing several separately would only be several chances for
		// them to disagree.
		//
		// It is still stored per slot rather than once, because slots do not
		// refresh together: one that has never rendered has nothing to compare
		// against, and one that appeared this frame has to draw once before it
		// can be skipped.
		// The frame clock, which is what a rate cap is measured against.
		//
		// **`SetAnimationTime`'s, because the renderer already has exactly one
		// idea of what time it is** and a second clock read here would let a
		// surface's interval drift against the flipbooks in it.
		frameSeconds = State->AnimationSeconds;

		surfaceSignature = 0;
		refreshCount = 0;
		if (wantSurface) {
			ENGINE_PROFILE_CAT("surface signature", core::ProfileCategory::Render);

			// Reuse the presentation boundary's complete view inputs, including
			// lighting and fitted surface cameras, rather than signing rows alone.
			ScenePresentationState mirrorInputs{};
			mirrorInputs.Lighting = Owner.CurrentLighting();
			mirrorInputs.Animation = Owner.TextureAnimationSignature(State->AnimationSeconds);
			// Primary-eye exclusion cannot change a secondary camera's pixels.
			auto surfaceSource = source;
			surfaceSource.EyeRig = 0;
			surfaceSource.EyePlayer.reset();
			surfaceSource.EyeHiddenRows = {};
			surfaceSignature = ScenePresentationSignature(surfaceSource, mirrorInputs);
			surfaceSignature = scene::MixSignature(surfaceSignature, ContentSignature);

			// **And how deep the mirrors are being drawn, which is an input to
			// every one of them.** A surface pass draws the *other* panes, so a
			// level added or taken away changes what is inside each picture as
			// surely as moving something does - and without this the automatic
			// depth could not climb at all in a still scene. It measures one
			// deeper, nothing else in the frame moves, no surface refreshes, the
			// deeper level is never drawn, and the next frame measures the same
			// shallow answer again. Found exactly that way, on
			// `MirrorCorridor.luau`, which is static on purpose.
			surfaceSignature = scene::MixSignature(surfaceSignature, surfaceBounces);

			// **And the other worlds, whose whole purpose is to be moving.** A
			// destination that changed while this world sat still is the case a
			// live portal exists for, and it is invisible to the line above.
			surfaceSignature = scene::MixSignature(surfaceSignature, scene::SignatureOf(foreign));

			for (size_t index = 0; index < acceptedCount; index++) {
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].Index);
				surfaceSignature = MixMatrix(surfaceSignature, accepted[index].ViewProjection);
				surfaceSignature = MixFloat(surfaceSignature, accepted[index].ImageOpacity);

				// **The foreign range, because moving it is a change nothing
				// else here would notice.** `SignatureOf(instances)` already
				// covers the *contents* of the appended tail - the far world
				// moving redraws the pane, which is the whole point of a live
				// destination - but a host that reordered two foreign ranges
				// without changing either world would leave the signature
				// identical while each surface now names the other's instances.
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].View->InstanceFirst);
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].View->InstanceCount);
			}

			for (size_t index = 0; index < acceptedCount; index++) {
				Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];

				// **Written whether or not the surface renders.** The opacity is
				// what the *screen* pass composites the pane with and it changes
				// no texel of the texture, so a mirror faded by a script fades
				// this frame rather than on whichever later frame something else
				// happens to move.
				state.ImageOpacity = accepted[index].ImageOpacity;
				state.Effect = accepted[index].Effect;

				// **Three independent reasons to skip, and they are not
				// interchangeable.** The signature says the image has not
				// changed; visibility says nothing is looking at it; the rate
				// says it drew recently enough. A surface has to clear all
				// three, and each of them alone leaves the slot holding the
				// frame it has along with the matrices that drew it.
				//
				// **Visibility overrides the never-rendered case.** A slot that
				// has never drawn has nothing to compare a signature against,
				// but it also has nothing looking at it - and `Ready` staying
				// false is exactly right for that: the pane draws as its own
				// tint until the frame something can see it.
				//
				// **The rate does not**, and that asymmetry is deliberate: a
				// surface must draw *once* as soon as something can see it, or
				// a pane walked up to shows its own tint for up to an interval
				// before the picture appears.
				// These live draw streams have no complete retained-input epoch here.
				// A present stream must refresh even while geometry and camera are still.
				const bool liveStream =
					gameInterfaceHook != nullptr || !Request.Particles.empty() || !Request.RibbonRuns.empty();
				const bool changed = !state.Ready || state.Signature != surfaceSignature || liveStream;
				const bool due = DueToDraw(state.Drawn, accepted[index].View->FPS, frameSeconds);

				accepted[index].Refresh =
					(sharedCaptures || surfaceVisible[accepted[index].Index]) && changed && due;

				refreshCount += accepted[index].Refresh ? 1u : 0u;
			}
		}

		// **This world's rows, then the other worlds' - one buffer, two halves
		// that never mix.** The head is what every pass in this frame partitions
		// and submits; the tail exists only so a surface can name a range of it.
		// Joining them before the plan is what drew two rooms on top of each
		// other until v0.14, and is why `foreign` is its own argument.
		{
			ENGINE_PROFILE_CAT("copy scene instances", core::ProfileCategory::Render);
			State->SceneInstances.assign(instances.begin(), instances.end());
			State->SceneJointFrames.assign(Request.JointFrames.begin(), Request.JointFrames.end());
		}
		ownCount = static_cast<uint32_t>(State->SceneInstances.size());
		{
			ENGINE_PROFILE_CAT("append/validate scene skins", core::ProfileCategory::Render);
			State->SceneInstances.insert(State->SceneInstances.end(), foreign.begin(), foreign.end());
			const uint32_t foreignJointBase = static_cast<uint32_t>(State->SceneJointFrames.size());
			State->SceneJointFrames.insert(
				State->SceneJointFrames.end(),
				Request.ForeignJointFrames.begin(),
				Request.ForeignJointFrames.end()
			);
			for (uint32_t index = 0; index < State->SceneInstances.size(); index++) {
				scene::DrawInstance &instance = State->SceneInstances[index];
				const size_t available =
					index < ownCount ? Request.JointFrames.size() : Request.ForeignJointFrames.size();
				const uint64_t end = static_cast<uint64_t>(instance.SkinFirst) + instance.SkinCount;
				if (instance.SkinCount == 0 || end > available) {
					instance.SkinFirst = 0;
					instance.SkinCount = 0;
				} else if (index >= ownCount) {
					instance.SkinFirst += foreignJointBase;
				}
			}
		}

		// **Every range the three scene passes submit, from one call.** The
		// ordering, the mirror partition and the caster partition are arithmetic
		// over a `shared` type and they live in `scene::OrderScene` - where a
		// headless suite can get at them. A renderer is the one module a test
		// cannot exercise, so the counts it hands to a draw call are the last
		// place they should be computed. See `scene::ScenePlan` for the runs.
		//
		// **Given the head alone.** A plan over the tail as well would sort
		// another world's parts into this one's opaque run, its mirror partition
		// and its shadow casters - and the ordering it returns is a permutation,
		// so it would also move the very rows a surface has already named.
		{
			ENGINE_PROFILE_CAT("order scene", core::ProfileCategory::Render);
			const auto started =
				profileCpuNodes ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			plan = scene::OrderScene(
				std::span<const scene::DrawInstance>(State->SceneInstances).first(ownCount),
				sceneEye,
				State->SceneOrder
			);
			if (profileCpuNodes) {
				if (const graph::Node *worldNode = graphNode(core::Name("world")); worldNode != nullptr) {
					cpuNodeWall[worldNode->Name.Id()] +=
						std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
							.count();
				}
			}
		}

		sceneCount = static_cast<uint32_t>(State->SceneInstances.size());
		sceneOpaque = static_cast<size_t>(plan.Opaque);
		sceneTransparent = plan.Transparent;
		sceneReflected = plan.Reflected;
		reflectedCasters = plan.ReflectedCasters;
		surfaceCasters = plan.SurfaceCasters;

		{
			// Allocation, on the frame an overlay first appears or changes size.
			// Zero on every other frame, which is what makes a reading here
			// worth looking at rather than background noise.
			// HasContent, not IsDirty. The texture keeps the last thing uploaded
			// to it, so a frame that redraws nothing still has a panel to show -
			// which is the whole point of the image living on the GPU rather
			// than being pushed there again every frame.
			// **Headless first, because nothing headless can show it.** The
			// overlay pass is the window's, so a headless frame allocated a
			// texture, copied the panels into it and drew none of them - and
			// `MarkUploaded` then told the image the GPU matched it, which was
			// a claim about a texture nothing would ever sample. Now the whole
			// overlay is one question answered once.
			//
			// Safe to skip only because the screen pass no longer borrows this
			// texture when the shadow map is missing; see `FallbackTexture`.
			ENGINE_PROFILE_CAT("ensure overlay", core::ProfileCategory::Render);
			haveOverlay = graphEnabled(core::Name("overlay")) && !State->Headless() && overlay.HasContent() &&
						  !overlay.IsEmpty() && State->EnsureOverlay(overlay.GetWidth(), overlay.GetHeight());
		}

		instanceCount = static_cast<uint32_t>(visibleCount);
		result.Culled = totalCount - instanceCount;

		// **One buffer, two ranges.** The scene range first so its offset is
		// zero and the camera range starts where it ends - which is what makes
		// each pass one `first_instance` rather than a second buffer and a
		// second bind.
		uploadCount = sceneCount + instanceCount;

		// Whether this view's pipeline authored `culling = "occlusion"` on its
		// entity filter, and the backend can serve it this frame. The CPU
		// frustum cull already ran - occlusion composes behind it rather than
		// replacing it.
		occlusionCulling = false;
		if (const Impl::InstalledPipeline *active = State->PipelineFor(State->ActiveGraph);
			active != nullptr && State->Occlusion.Seed != nullptr && State->Occlusion.Reduce != nullptr &&
			State->Occlusion.Cull != nullptr && State->Occlusion.Args != nullptr) {
			for (uint32_t value = 1; value <= active->Graph.Count() && !occlusionCulling; value++) {
				const graph::Node *node = active->Graph.Find(graph::NodeId{value});
				if (node == nullptr || !node->Enabled || node->Kind != core::Name("cull-frustum")) {
					continue;
				}
				const std::string *culling = node->Parameter(core::Name("culling"));
				occlusionCulling = culling != nullptr && *culling == "occlusion";
			}
		}

		PackInstances();

		// The particles, packed and grouped before any render pass opens.
		//
		// **Here rather than inside the draw**, because what follows is a copy
		// pass and a copy pass cannot be started while a render pass is open -
		// the same constraint `FrameOverlayHook`'s `Prepare`/`Record` split
		// exists for, stated in that header in the same words.
		// The local lights, packed once for the frame.
		//
		// **Every pass gets the same set**, including a mirror's: a lamp lights
		// what a reflection shows exactly as it lights the world, and giving the
		// surface pass a different set would make a mirror disagree with the room
		// it is in.
		lightUniforms = ToGpu(lights);
		SceneLightIds.fill({});
		SceneLightShadows.fill(false);
		for (size_t index = 0; index < std::min(lights.size(), MAX_SCENE_LIGHTS); ++index) {
			SceneLightIds[index] = lights[index].DataFactoryId;
			SceneLightShadows[index] = lights[index].Shadows;
		}

		// Begin the device timeline before particle preparation because its copy,
		// scatter and simulation now belong to this frame command buffer. The old
		// standalone particle submission sat outside the frame's timestamp pool
		// and made its device cost invisible.
		if (State->BatchFirst || !State->BatchActive) {
			State->CollectTimings();
			State->WallTimings.clear();
			State->DroppedProfileMarks = 0;
		}
		// Split owner commands use the controller's retained per-recorder queries;
		// the ordinary pool cannot recycle after an unproven prefix failure.
		const bool sampleGpu = !State->GpuQueues.Active() &&
							   ((State->ProfileTier == ProfilingTier::Full && State->ProfileSampleRate > 0 &&
								 State->FrameCounter % State->ProfileSampleRate == 0) ||
								State->BatchCaptureTimingRequested);
		timingSlot = !sampleGpu ? VulkanTimestamps::NO_SLOT
					 : State->BatchActive
						 ? (State->BatchFirst ? State->Timestamps.Begin(command) : State->BatchTimingSlot)
						 : State->Timestamps.Begin(command);
		if (State->BatchActive && State->BatchFirst) {
			State->BatchTimingSlot = timingSlot;
		}
		if ((State->BatchFirst || !State->BatchActive) && timingSlot < VulkanTimestamps::SLOTS) {
			State->PendingMarks[timingSlot].clear();
			State->PendingCaptureTimings[timingSlot].clear();
		}
		openedMark = VulkanTimestamps::MARKS;
		timedCommand = command;
		openedWall = std::chrono::steady_clock::now();
		mainGpuWorkRecorded = State->BatchActive && !State->BatchFirst;
		dedicatedComputeSubmitted = false;

		particleCount = 0;
		ribbonCount = 0;
		{
			ENGINE_PROFILE_CAT("prepare particles", core::ProfileCategory::Render);
			const bool effectsVisible =
				graphEnabled(core::Name("transparent")) || graphEnabled(core::Name("transparent-layer"));
			const Impl::ParticlePreparation prepared =
				effectsVisible ? State->PrepareParticles(source, command, timingSlot)
							   : Impl::ParticlePreparation{};
			const Impl::GpuParticleFieldPreparation fieldPrepared =
				effectsVisible ? State->PrepareGpuParticleFields(source, command, timingSlot)
							   : Impl::GpuParticleFieldPreparation{};
			particleCount = prepared.Count + fieldPrepared.Count;
			result.ComputeDispatches += prepared.Dispatches + fieldPrepared.Dispatches;
			result.Particles = particleCount;

			ribbonCount = effectsVisible ? State->PrepareRibbons(ribbonVertices) : 0;
			result.RibbonVertices = ribbonCount;
		}

		// Only when something is actually waiting to go across. A panel redrawn
		// ten times a second and presented a thousand times has nothing to
		// upload on nine hundred and ninety of those frames.
		uploadOverlay = haveOverlay && (State->OverlayUninitialised || overlay.UploadRegion().Width > 0);

		// --- shadow pass ----------------------------------------------------
		//
		// **The scene range, not the camera's**, and no colour target at all.
		// Every caster casts, whether or not the eye can see it.
		//
		// A scene whose opaque geometry all opted out of casting skips the pass
		// rather than clearing a depth target nothing writes to - and the
		// colour pass then samples a shadow map that was never rendered, which
		// is what `FrameResult::Ran` exists to make visible. An explicit capture still
		// clears the empty map, so stale casters cannot enter its returned depth.
		const bool captureShadow = State->HasShadowCaptureRequest(pipeline, request.TargetSlot);
		haveShadow = graphEnabled(core::Name("shadow")) &&
					 (captureShadow || source.ImportedDirectionalShadow != 0 ||
					  (haveInstances && sceneCount > 0 && (reflectedCasters > 0 || surfaceCasters > 0))) &&
					 State->EnsureShadow();

		// Builds the per-draw block from world lighting and the camera used by
		// this pass. Fog is eye-relative, so a reflected or portal sub-view must
		// not reuse the screen eye even though every other authored term is shared.
		currentLighting = Owner.CurrentLighting();

		State->Beams = BeamUniforms{};

		// The interface upload must precede every scene pass that can draw a
		// spatial collector. `Prepare` opens a copy pass and therefore cannot run
		// once a mirror, surface or portal render pass is open. Headless frames
		// still draw into capture targets; a hook that has no backend declines in
		// `Prepare` itself.
		const bool prepareInterface =
			(Request.Damage.GameInterface || Request.Damage.Scene) &&
			(graphEnabled(core::Name("interface")) || graphEnabled(core::Name("transparent-layer")) ||
			 graphEnabled(core::Name("spatial-overlay"))) &&
			gameInterfaceHook != nullptr;
		if (prepareInterface) {
			drawInterface = gameInterfaceHook->Prepare(command);
			SnapshotPreparationStatus = gameInterfaceHook->PreparationStatus();
			if (!drawInterface && Request.Source->SpatialSnapshotSequence != 0 &&
				SnapshotPreparationStatus == OverlayRecordStatus::Complete &&
				gameInterfaceHook->PreparedEmpty())
				drawInterface = true;
		}
		if (Request.Source->SpatialSnapshotSequence != 0 && Request.Source->SnapshotLayersKnown) {
			SnapshotLayerStatus = OverlayRecordStatus::Complete;
			const auto requireLayer = [&](OverlayLayer layer) {
				const auto availability = gameInterfaceHook ? gameInterfaceHook->LayerAvailability(layer)
															: OverlayRecordStatus::Failed;
				if (availability == OverlayRecordStatus::Failed ||
					(availability == OverlayRecordStatus::Unknown &&
					 SnapshotLayerStatus == OverlayRecordStatus::Complete))
					SnapshotLayerStatus = availability;
			};
			if (Request.Source->SnapshotScreenCommands) requireLayer(OverlayLayer::Screen);
			if (Request.Source->SnapshotSpatialCommands) requireLayer(OverlayLayer::Spatial);
			const auto matchContent = [&](OverlayLayer layer, bool expected) {
				const auto content =
					gameInterfaceHook ? gameInterfaceHook->LayerContent(layer) : OverlayLayerContent::Empty;
				if (content == OverlayLayerContent::Invalid ||
					(content == OverlayLayerContent::Present && !expected) ||
					(content == OverlayLayerContent::Empty && expected))
					SnapshotLayerStatus = OverlayRecordStatus::Failed;
				else if (content == OverlayLayerContent::Unknown &&
						 SnapshotLayerStatus == OverlayRecordStatus::Complete)
					SnapshotLayerStatus = OverlayRecordStatus::Unknown;
			};
			matchContent(OverlayLayer::Screen, Request.Source->SnapshotScreenCommands);
			matchContent(OverlayLayer::Spatial, Request.Source->SnapshotSpatialCommands);
		}
		drawHostOverlay =
			swapchain != nullptr && hostOverlayHook != nullptr && hostOverlayHook->Prepare(command);

		// --- the mirror recursion --------------------------------------------
		//
		// **What a pane shows a viewer that is not the eye.**
		//
		// `scene::AimSurfaceCameras` places every surface camera by reflecting the
		// world's *active* camera through its pane. That answer is right for the
		// screen and wrong everywhere else: a pane appearing inside another pane's
		// picture is being looked at from that pane's camera, several studs and a
		// reflection away from the eye. Projecting it with the eye's matrix put
		// the coordinate outside the texture's 0..1 rectangle, and `opaque.frag`
		// falls back to the plain lit pane there - which is the flat slab a mirror
		// seen in a mirror used to be, and which reads as culling rather than as a
		// projection fault.
		//
		// **Running the pass again cannot fix it, and that is worth being precise
		// about.** Iterating refreshes textures; it never moves a camera. Each
		// bounce redrew the same eye-derived viewpoints with fresher contents, so
		// the chain got newer and stayed wrong.
		//
		// So the levels below the first are a recursion, exactly as a hole's are:
		// each level's camera is `scene::ReflectCamera` applied to the camera of
		// the level above, and reflections therefore compose by construction. The
		// two passes now differ in what a level's camera *is* and in nothing else,
		// which is why they share `openScenePass`, `drawWorldInto` and
		// `drawBlendedInto` and keep their own entry points.
		//
		// **Depth first, and every level's targets survive until the level above
		// has drawn all of its panes** - `Impl::MirrorLevel` is indexed by level
		// and slot for that reason, which is `Impl::PortalLevel`'s reason.
		//
		// **One fewer than the bounce count, because the surface pass is the top
		// level.** `SetSurfaceBounces(2)` has always meant "two levels of
		// mirror-in-mirror", and it still does: the pane's own texture is one and
		// the recursion supplies the rest. Zero here is one bounce - no
		// recursion, every inner pane drawn flat - which is exactly what one
		// bounce drew before and is the honest floor rather than a special case.
		//
		// **The subtraction cannot underflow because every path to
		// `surfaceBounces` floors at one**, which is why that is stated at each
		// of them rather than defended again here.
		mirrorLevels = surfaceBounces - 1u;

		// **Cleared for the whole bank before anything descends.** A target holds
		// last frame's picture from a camera that no longer exists, and a level
		// that is not reached this frame must read as absent rather than as stale
		// - see `Impl::MirrorTarget::Ready`.
		for (Impl::MirrorLevel &level : bank.Mirrors) {
			for (Impl::MirrorTarget &target : level.Targets) {
				target.Ready = false;
			}
		}

		// --- view targets ---------------------------------------------------

		// **The world's target, which is the offscreen texture or the window.**
		// Graph passes continue through this target. Host chrome is recorded only
		// after `output-image`, so it cannot leak into graph previews or captures.
		colourTarget.texture = offscreen ? State->SlotAt(targetSlot).Texture : swapchain;
		colourTarget.clear_color = SDL_FColor{
			State->FogColour.r,
			State->FogColour.g,
			State->FogColour.b,
			1.0f,
		};
		colourTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		colourTarget.store_op = SDL_GPU_STOREOP_STORE;

		// What the overlay and the interface draw onto. When the world went
		// offscreen the window has never been touched this frame, so the first
		// pass to reach it clears - otherwise it is whatever the driver handed
		// back, which is last frame's image or uninitialised memory.
		windowTarget.texture = swapchain;
		windowTarget.clear_color = colourTarget.clear_color;
		windowTarget.load_op = offscreen ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		windowTarget.store_op = SDL_GPU_STOREOP_STORE;

		// The one `EnsureDepth` above filled: this slot's when the world is going
		// into a texture, the shared window one when it is going to the
		// swapchain. See `SceneSlot::Depth`.
		depthTarget.texture = offscreen ? State->SlotAt(targetSlot).Depth : State->DepthTexture;
		depthTarget.clear_depth = 1.0f;
		depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		// Graph consumers sample the completed opaque depth after the gbuffer pass.
		depthTarget.store_op = SDL_GPU_STOREOP_STORE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.cycle = true;

		// --- default PBR graph ---------------------------------------------
		//
		// The material-producing head and its explicit consumers. Projected
		// surfaces and blended geometry are submitted by the graph's transparent
		// node over the finished image.
		const auto outputDimensions = [&](core::Name kind,
										  size_t output,
										  uint32_t &outWidth,
										  uint32_t &outHeight,
										  core::Name port = {}) {
			ENGINE_PROFILE_CAT("resolve pipeline output extent", core::ProfileCategory::Render);
			outWidth = sceneWidth;
			outHeight = sceneHeight;
			const graph::Node *node = selectedPipeline->NodeLookup.First(selectedPipeline->Graph, kind);
			if (node == nullptr) return;
			if (port.IsValid()) {
				const auto found = std::find(node->WritePorts.begin(), node->WritePorts.end(), port);
				if (found == node->WritePorts.end()) return;
				output = static_cast<size_t>(found - node->WritePorts.begin());
			}
			if (output >= node->Writes.size()) return;
			const graph::ResourceDesc *resource =
				selectedPipeline->Graph.FindResourceDesc(node->Writes[output]);
			if (resource != nullptr) {
				resource->Resolve(sceneWidth, sceneHeight, outWidth, outHeight);
			}
		};
		pbrDimensions.TargetWidth = targetWidth;
		pbrDimensions.TargetHeight = targetHeight;
		pbrDimensions.ViewWidth = sceneWidth;
		pbrDimensions.ViewHeight = sceneHeight;
		pbrDimensions.LinearWidth = sceneWidth;
		pbrDimensions.LinearHeight = sceneHeight;
		pbrDimensions.OcclusionWidth = sceneWidth;
		pbrDimensions.OcclusionHeight = sceneHeight;
		pbrDimensions.LitWidth = sceneWidth;
		pbrDimensions.LitHeight = sceneHeight;
		pbrDimensions.FirstSurfaceValidity = graphEnabled(core::Name("depth-validity"));
		pbrDimensions.SecondSurface = graphEnabled(core::Name("depth-peel"));
		pbrDimensions.CameraMotion = graphEnabled(core::Name("camera-motion"));
		pbrDimensions.HalfOcclusion =
			currentLighting.HalfResolutionAmbientOcclusion && graphEnabled(core::Name("ssao"));
		outputDimensions(
			core::Name("depth-linearise"), 0, pbrDimensions.LinearWidth, pbrDimensions.LinearHeight
		);
		outputDimensions(core::Name("ssao"), 0, pbrDimensions.OcclusionWidth, pbrDimensions.OcclusionHeight);
		// Optional lighting outputs do not choose the primary colour extent.
		outputDimensions(
			core::Name("deferred-lighting"),
			0,
			pbrDimensions.LitWidth,
			pbrDimensions.LitHeight,
			core::Name("colour")
		);
		const bool needsPbrTargets =
			graphEnabled(core::Name("gbuffer")) || graphEnabled(core::Name("depth-peel")) ||
			graphEnabled(core::Name("depth-linearise")) || graphEnabled(core::Name("depth-validity")) ||
			graphEnabled(core::Name("ssao")) || graphEnabled(core::Name("deferred-lighting")) ||
			graphEnabled(core::Name("camera-motion")) || graphEnabled(core::Name("fog")) ||
			graphEnabled(core::Name("tonemap")) || graphEnabled(core::Name("transparent"));
		bool graphTargetsReady = true;
		if (needsPbrTargets) {
			ENGINE_PROFILE_CAT("ensure pbr targets", core::ProfileCategory::Render);
			graphTargetsReady = State->EnsurePbr(targetSlot, pbrDimensions);
		}
		if (!graphTargetsReady) {
			closePass();
			State->Timestamps.Abandon(timingSlot);
			if (timingSlot < VulkanTimestamps::SLOTS) {
				State->PendingMarks[timingSlot].clear();
				State->AbandonCaptureTimings(timingSlot);
			}
			endIncompleteView();
			return ViewStart::Abandoned;
		}
		Pbr = &State->PbrAt(targetSlot);
		Impl::PbrSlot &pbr = *Pbr;
		viewTarget = colourTarget.texture;

		// Named here because the transparent node reads it and that lambda has
		// a `source` of its own - a texture, not this view.
		frameUniforms = FrameUniforms{
			matrices.ViewProjection,
			lightViewProjection,
			glm::mat4{1.0f},
		};
		lighting = LightingAt(behaviourFrame.Position, 0.0f, 0.0f);

		uniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
		uniforms.LightViewProjection = lightViewProjection;
		uniforms.Planes = glm::vec4{
			drawCamera.NearPlane,
			drawCamera.FarPlane,
			drawCamera.NearPlane > 0.0f ? 1.0f / drawCamera.NearPlane : 0.0f,
			drawCamera.FarPlane > 0.0f ? 1.0f / drawCamera.FarPlane : 0.0f,
		};
		uniforms.Target = glm::vec4{
			static_cast<float>(sceneWidth),
			static_cast<float>(sceneHeight),
			static_cast<float>(sceneWidth) / static_cast<float>(targetWidth),
			static_cast<float>(sceneHeight) / static_cast<float>(targetHeight),
		};
		uniforms.Direction = glm::vec4{State->Sun, 0.0f};
		uniforms.Ambient = State->Ambient;
		uniforms.OutdoorAmbient = State->OutdoorAmbient;
		uniforms.Direct = State->Direct;
		uniforms.Eye =
			glm::vec4{behaviourFrame.Position.X, behaviourFrame.Position.Y, behaviourFrame.Position.Z, 1.0f};
		const core::Vector3 forward = behaviourFrame.LookVector();
		uniforms.CameraDepth =
			glm::vec4{forward.X, forward.Y, forward.Z, -forward.Dot(behaviourFrame.Position)};
		uniforms.FogColour = glm::vec4{
			WorkingFromDisplay(State->FogColour.r),
			WorkingFromDisplay(State->FogColour.g),
			WorkingFromDisplay(State->FogColour.b),
			1.0f,
		};
		uniforms.Fog = glm::vec4{State->FogStart, State->FogEnd, 0.0f, 0.0f};
		uniforms.Shadow = glm::vec4{
			haveShadow ? 1.0f : 0.0f,
			1.0f / static_cast<float>(SHADOW_RESOLUTION),
			0.0f,
			0.0f,
		};
		uniforms.VolumeCount = glm::vec4{static_cast<float>(currentLighting.VolumeCount), 0.0f, 0.0f, 0.0f};
		if (const auto &cloud = source.CloudDensity; cloud && cloud->IsValid()) {
			uniforms.CloudMinimumNodeCount = glm::vec4{
				cloud->Config.RootMinimum.X,
				cloud->Config.RootMinimum.Y,
				cloud->Config.RootMinimum.Z,
				static_cast<float>(cloud->Nodes.size())
			};
			uniforms.CloudSizeDepth = glm::vec4{
				cloud->Config.RootSize.X,
				cloud->Config.RootSize.Y,
				cloud->Config.RootSize.Z,
				static_cast<float>(cloud->Config.MaximumDepth)
			};
			uniforms.CloudCentreEnabled = glm::vec4{cloud->Centre.X, cloud->Centre.Y, cloud->Centre.Z, 1.0f};
		}
		for (size_t index = 0; index < currentLighting.VolumeCount; index++) {
			const scene::VolumeState &volume = currentLighting.Volumes[index];
			PbrUniforms::VolumeUniform &out = uniforms.Volumes[index];
			const core::Vector3 x = volume.Frame.RightVector();
			const core::Vector3 y = volume.Frame.UpVector();
			const core::Vector3 z = volume.Frame.ZVector();
			out.Origin = glm::vec4{
				volume.Frame.Position.X,
				volume.Frame.Position.Y,
				volume.Frame.Position.Z,
				volume.Falloff,
			};
			out.AxisX = glm::vec4{
				x.X,
				x.Y,
				x.Z,
				volume.Shape == scene::VolumeShape::Ellipsoid ? 1.0f : 0.0f,
			};
			out.AxisY = glm::vec4{y.X, y.Y, y.Z, 0.0f};
			out.AxisZ = glm::vec4{z.X, z.Y, z.Z, volume.HalfExtent.Z};
			out.ColourDensity = glm::vec4{volume.Colour.R, volume.Colour.G, volume.Colour.B, volume.Density};
			out.ExtinctionNoise = glm::vec4{
				volume.Extinction,
				volume.NoiseScale,
				volume.NoiseStrength,
				static_cast<float>(volume.Seed),
			};
			out.Steps = glm::vec4{
				static_cast<float>(volume.Steps),
				static_cast<float>(volume.ShadowSteps),
				volume.HalfExtent.X,
				volume.HalfExtent.Y,
			};
		}

		LensGroupCount = 0;
		size_t lensCount = 0;
		LensPassData = {};
		LensPassData.ViewProjection = matrices.ViewProjection;
		LensPassData.InverseViewProjection = uniforms.InverseViewProjection;
		LensPassData.Target = uniforms.Target;
		LensPassData.Eye = uniforms.Eye;
		LensPassData.CameraDepth = uniforms.CameraDepth;
		LensPassData.TimeCount.x = Request.Source->LensTimeSeconds.value_or(static_cast<float>(frameSeconds));
		const graph::Frustum lensFrustum = graph::Frustum::FromViewProjection(matrices.ViewProjection);
		for (size_t index = 0; index < currentLighting.ShaderLensCount; index++) {
			const scene::ShaderLensState &lens = currentLighting.ShaderLenses[index];
			if (!lensFrustum.Intersects(lens.Frame.Position, lens.Radius)) {
				continue;
			}

			const bool startsRun =
				LensGroupCount == 0 || LensGroups[LensGroupCount - 1].Shader != lens.Shader;
			LensGroup *group = nullptr;
			if (startsRun) {
				group = &LensGroups[LensGroupCount++];
				*group = LensGroup{.Shader = lens.Shader, .First = lensCount};
			} else {
				group = &LensGroups[LensGroupCount - 1];
			}

			LensPassUniforms::LensUniform &out = LensEntries[lensCount++];
			group->Count++;
			const core::Vector3 x = lens.Frame.RightVector();
			const core::Vector3 y = lens.Frame.UpVector();
			const core::Vector3 z = lens.Frame.ZVector();
			out.CentreRadius =
				glm::vec4{lens.Frame.Position.X, lens.Frame.Position.Y, lens.Frame.Position.Z, lens.Radius};
			out.AxisXInner = glm::vec4{x.X, x.Y, x.Z, lens.InnerRadius};
			out.AxisYFalloff = glm::vec4{y.X, y.Y, y.Z, lens.Falloff};
			out.AxisZStrength = glm::vec4{z.X, z.Y, z.Z, lens.Strength};
			out.SpinPriority = glm::vec4{lens.Spin, static_cast<float>(lens.Priority), 0.0f, 0.0f};
		}
		sceneViewport = SDL_GPUViewport{
			0.0f,
			0.0f,
			static_cast<float>(sceneWidth),
			static_cast<float>(sceneHeight),
			0.0f,
			1.0f,
		};
		sceneScissor = SDL_Rect{0, 0, static_cast<int>(sceneWidth), static_cast<int>(sceneHeight)};

		sampler = State->SurfaceSampler;
		depthBindings = {SDL_GPUTextureSamplerBinding{depthTarget.texture, sampler}};

		lightingBindings = {
			SDL_GPUTextureSamplerBinding{pbr.Albedo, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Normal, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Material, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Emissive, sampler},
			SDL_GPUTextureSamplerBinding{pbr.LinearDepth, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Occlusion, sampler},
			SDL_GPUTextureSamplerBinding{
				State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture,
				State->ShadowSampler != nullptr ? State->ShadowSampler : sampler,
			},
		};

		return ViewStart::Recording;
	}
}
