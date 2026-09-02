// What one view works out before any pass records, and what it does once they
// all have.
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

#include "ViewRecording.hpp"

#include "DisplayColour.hpp"
#include "SurfaceScale.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

namespace engine::render {

	namespace {
		template <typename Value>
		void StageBulk(
			Value *destination, std::span<const InstanceUploadRange> ranges, std::span<const Value> source
		) {
			if (ranges.empty()) {
				return;
			}
			uint32_t first = ranges.front().First;
			uint32_t last = first + ranges.front().Count;
			for (const InstanceUploadRange &range : ranges) {
				first = std::min(first, range.First);
				last = std::max(last, range.First + range.Count);
			}
			std::memcpy(
				destination + first, source.data() + first, static_cast<size_t>(last - first) * sizeof(Value)
			);
		}
	}

	ViewStart ViewRecording::Begin(const ViewRequest &request) {
		ENGINE_PROFILE_CAT("ViewRecording::Begin", core::ProfileCategory::Render);

		Request = request;
		Instances = request.Instances;
		Foreign = request.Foreign;

		// **The names the body below works in, bound to what the recording
		// keeps.** Each one *is* the member rather than a copy of it, which is
		// what makes a value the passes read impossible to forget to publish:
		// there is nothing to publish. See `src/nodes/` for the other half of
		// the same arrangement.
		Impl *const State = this->State;
		FrameResult &result = Result;
		const core::CFrame &cameraFrame = Request.CameraFrame;
		const scene::Camera &camera = Request.Camera;
		OverlayImage &overlay = *Request.Overlay;
		const std::span<const SurfaceView> surfaces = Request.Surfaces;
		FrameOverlayHook *const gameInterfaceHook = Request.GameInterfaceHook;
		FrameOverlayHook *const hostOverlayHook = Request.HostOverlayHook;
		const SceneTarget *const sceneTarget = Request.Target;
		const size_t targetSlot = Request.TargetSlot;
		const View &source = *Request.Source;
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
		auto &cameraMatrix = CameraMatrix;
		auto &matrices = Matrices;
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
		auto &tonemapBindings = TonemapBindings;
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
		// **Measured off the panes this frame was handed rather than off the
		// world**, because that is the same set the pass below draws through and
		// the renderer has no store to ask. `scene::NearestSeamDistance` answers
		// the same question from a store, from the same functions, for a caller
		// that has one.
		nearestPane = std::numeric_limits<float>::infinity();
		for (const PortalView &portal : portals) {
			nearestPane = std::min(
				nearestPane,
				scene::RectangleDistance(portal.Centre, portal.First, portal.Second, cameraFrame.Position)
			);
		}

		// **One adapted copy used by every projection this frame builds**, so the
		// cull, the portal recursion and the opaque draw cannot disagree about
		// where the near plane is. A cull run against a larger near plane than
		// the draw uses throws away exactly the geometry the smaller one exists
		// to keep.
		drawCamera = camera;
		drawCamera.NearPlane = scene::PortalNearPlane(camera.NearPlane, nearestPane);

		// **Claimed here only if the caller did not claim it first.** `WaitForFrame`
		// is what a latency-sensitive loop calls before it reads its input; a
		// caller that does not is no worse off than before, because this is the
		// same acquisition at the same point in the frame. See `Impl::BeginFrame`.
		if (State->BatchActive) {
			command = State->BatchCommand;
			if (State->BatchFinal) {
				swapchain = State->BatchSwapchain;
				width = State->BatchWidth;
				height = State->BatchHeight;
			}
		} else if (present) {
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
		// A target that cannot be allocated falls back to the window rather than
		// dropping the frame. A caller asking for a texture and getting a frame
		// it did not expect can see that something is wrong; one that gets no
		// frame at all sees a frozen editor.
		offscreen = sceneTarget != nullptr && sceneTarget->IsValid() &&
					State->EnsureScene(sceneTarget->Width, sceneTarget->Height);

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
		// A frame where everything named is loaded copies the span and does one
		// hash lookup per instance, which is nothing beside the hundred and
		// fifty bytes of traffic per instance the collector already pays.
		{
			ENGINE_PROFILE_CAT("filter unloaded", core::ProfileCategory::Render);

			scene::KeepLoaded(
				instances,
				[State](const core::Name &mesh) { return State->Meshes.Has(mesh); },
				State->Drawable
			);

			// **The other worlds pay the same toll.** A destination whose meshes
			// have not arrived here would otherwise come up as the field of
			// cubes described above - seen through a portal, which is the one
			// place a viewer cannot walk over and check.
			scene::KeepLoaded(
				foreign,
				[State](const core::Name &mesh) { return State->Meshes.Has(mesh); },
				State->DrawableForeign
			);
		}
		instances = State->Drawable;
		foreign = State->DrawableForeign;

		// --- uploads --------------------------------------------------------

		const auto totalCount = static_cast<uint32_t>(instances.size());
		haveInstances = false;
		haveOverlay = false;

		// **Culled, then ordered, then uploaded** - and the sequence is the
		// point. Culling first means the sort runs over what survives rather
		// than over the world, and the upload carries only what is drawn.
		//
		// The frustum comes from the same `ResolveCamera` the draw does, so it
		// cannot disagree with what was actually projected. A frustum built from
		// a field of view and an aspect ratio kept separately is the bug that
		// pops geometry at the screen edge on one machine and not another.

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
		portalLevels = std::min(State->PortalDepth, MAX_PORTAL_DEPTH);

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
			pane.Surface = static_cast<int8_t>(accepted[index].Index);
			pane.TagFilter = view.TagFilter;
			pane.NearPlane = view.PaneNear;
			pane.FarPlane = view.PaneFar;

			paneWidth[accepted[index].Index] = view.Width;
			paneHeight[accepted[index].Index] = view.Height;

			havePanes[accepted[index].Index] = true;
			anyPane = true;
		}

		const float cameraAspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
		cameraMatrix = scene::ResolveCamera(cameraFrame, drawCamera, cameraAspect).ViewProjection;
		bool sceneBoundsReady = false;

		graph::EntityFlow &entityFlow = State->GraphEntities;
		graph::Viewpoints &viewpoints = State->GraphViewpoints;
		entityFlow.Clear();
		viewpoints.Clear();
		graph::Viewpoint fallbackViewpoint;
		fallbackViewpoint.Frame = cameraFrame;
		fallbackViewpoint.Lens = drawCamera;

		size_t visibleCount = instances.size();
		opaqueCount = 0;
		core::Name orderedEntities;

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
				const auto started = std::chrono::steady_clock::now();
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
				if (run.BoundedAll) {
					sceneBounds = run.Bounds;
					sceneBoundsReady = true;
				}
				visibleCount = run.Output.IsValid() ? run.Count : visibleCount;
				if (run.Ordered) {
					opaqueCount = run.Opaque;
					orderedEntities = run.Output;
				}
				const auto ended = std::chrono::steady_clock::now();
				cpuNodeWall[node->Name.Id()] +=
					std::chrono::duration<double, std::micro>(ended - started).count();
			}
		}
		if (!sceneBoundsReady) {
			sceneBounds = graph::BoundsOfAll(instances);
		}

		const std::span<const uint32_t> ordered = entityFlow.Get(orderedEntities);
		{
			// Two full copies of the draw list, per view, per frame. Small on a
			// demo and not small on a scene, and nothing named them.
			ENGINE_PROFILE_CAT("copy draw lists", core::ProfileCategory::Render);
			State->VisibleInstances.assign(instances.begin(), instances.end());
			State->DrawOrder.assign(ordered.begin(), ordered.end());
		}
		visibleCount = ordered.size();

		// **Fitted to the whole draw list, not to what survived culling.** A
		// caster outside the camera's frustum still shadows into it, so the
		// light has to see everything - and fitting to the culled set is the
		// classic version of this bug: shadows that vanish as their casters
		// leave the screen. That is why `sceneBounds` is the union over
		// `instances` and not over `State->Visible`.
		//
		// The graph cull only changes what the view draws. Shadows still fit the
		// whole world so an off-screen caster cannot disappear from the map.
		lightViewProjection =
			graph::FitDirectionalLight(sceneBounds, core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z});
		transparentCount = static_cast<uint32_t>(visibleCount - opaqueCount);

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
				cameraMatrix,
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
		if (acceptedCount > State->SurfaceLimit) {
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

				if (State->EnsureSurface(
						targetSlot, view.Index, view.View->Width * scale, view.View->Height * scale
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
		sceneEye = wantSurface ? accepted[0].View->Frame.Position : cameraFrame.Position;

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

			surfaceSignature = scene::SignatureOf(instances);

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
				const bool changed = !state.Ready || state.Signature != surfaceSignature;
				const bool due = DueToDraw(state.Drawn, accepted[index].View->FPS, frameSeconds);

				accepted[index].Refresh = surfaceVisible[accepted[index].Index] && changed && due;

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
			const auto started = std::chrono::steady_clock::now();
			plan = scene::OrderScene(
				std::span<const scene::DrawInstance>(State->SceneInstances).first(ownCount),
				sceneEye,
				State->SceneOrder
			);
			if (const graph::Node *worldNode = graphNode(core::Name("world")); worldNode != nullptr) {
				cpuNodeWall[worldNode->Name.Id()] +=
					std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
						.count();
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
		if (const Impl::NamedPipeline *active = State->PipelineFor(State->ActiveGraph);
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

		// Begin the device timeline before particle preparation because its copy,
		// scatter and simulation now belong to this frame command buffer. The old
		// standalone particle submission sat outside the frame's timestamp pool
		// and made its device cost invisible.
		if (State->BatchFirst || !State->BatchActive) {
			State->CollectTimings();
			State->WallTimings.clear();
			State->DroppedProfileMarks = 0;
		}
		const bool sampleGpu = State->ProfileTier == ProfilingTier::Full && State->ProfileSampleRate > 0 &&
							   State->FrameCounter % State->ProfileSampleRate == 0;
		timingSlot = !sampleGpu ? VulkanTimestamps::NO_SLOT
					 : State->BatchActive
						 ? (State->BatchFirst ? State->Timestamps.Begin(command) : State->BatchTimingSlot)
						 : State->Timestamps.Begin(command);
		if (State->BatchActive && State->BatchFirst) {
			State->BatchTimingSlot = timingSlot;
		}
		if ((State->BatchFirst || !State->BatchActive) && timingSlot < VulkanTimestamps::SLOTS) {
			State->PendingMarks[timingSlot].clear();
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
			const Impl::ParticlePreparation prepared =
				graphEnabled(core::Name("transparent")) ? State->PrepareParticles(source, command, timingSlot)
														: Impl::ParticlePreparation{};
			particleCount = prepared.Count;
			result.ComputeDispatches += prepared.Dispatches;
			result.Particles = particleCount;

			ribbonCount = graphEnabled(core::Name("transparent")) ? State->PrepareRibbons(ribbonVertices) : 0;
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
		// is what `FrameResult::Ran` exists to make visible.
		haveShadow = graphEnabled(core::Name("shadow")) && haveInstances && sceneCount > 0 &&
					 (reflectedCasters > 0 || surfaceCasters > 0) && State->EnsureShadow();

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
		drawInterface = Request.Damage.GameInterface && graphEnabled(core::Name("interface")) &&
						gameInterfaceHook != nullptr && gameInterfaceHook->Prepare(command);
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
		// Nothing reads depth after the pass, so there is no reason to write it
		// back out to memory.
		depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.cycle = true;

		// --- default PBR graph ---------------------------------------------
		//
		// The material-producing head and its explicit consumers. Projected
		// surfaces and blended geometry are submitted by the graph's transparent
		// node over the finished image.
		const auto outputDimensions =
			[&](core::Name kind, size_t output, uint32_t &outWidth, uint32_t &outHeight) {
				outWidth = sceneWidth;
				outHeight = sceneHeight;
				const graph::Node *node = nullptr;
				for (uint32_t value = 1; value <= selectedPipeline->Graph.Count(); value++) {
					const graph::Node *candidate = selectedPipeline->Graph.Find(graph::NodeId{value});
					if (candidate != nullptr && candidate->Kind == kind) {
						node = candidate;
						break;
					}
				}
				if (node == nullptr || output >= node->Writes.size()) {
					return;
				}
				const graph::ResourceDesc *resource =
					selectedPipeline->Graph.FindResource(node->Writes[output]);
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
		outputDimensions(
			core::Name("depth-linearise"), 0, pbrDimensions.LinearWidth, pbrDimensions.LinearHeight
		);
		outputDimensions(core::Name("ssao"), 0, pbrDimensions.OcclusionWidth, pbrDimensions.OcclusionHeight);
		outputDimensions(core::Name("deferred-lighting"), 0, pbrDimensions.LitWidth, pbrDimensions.LitHeight);
		const bool needsPbrTargets =
			graphEnabled(core::Name("gbuffer")) || graphEnabled(core::Name("depth-linearise")) ||
			graphEnabled(core::Name("ssao")) || graphEnabled(core::Name("deferred-lighting")) ||
			graphEnabled(core::Name("tonemap")) || graphEnabled(core::Name("transparent"));
		const bool graphTargetsReady = !needsPbrTargets || State->EnsurePbr(targetSlot, pbrDimensions);
		if (!graphTargetsReady) {
			closePass();
			State->Timestamps.Abandon(timingSlot);
			if (timingSlot < VulkanTimestamps::SLOTS) {
				State->PendingMarks[timingSlot].clear();
			}
			endIncompleteView();
			return ViewStart::Abandoned;
		}
		Pbr = &State->PbrAt(targetSlot);
		Impl::PbrSlot &pbr = *Pbr;
		viewTarget = colourTarget.texture;
		const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
		matrices = scene::ResolveCamera(cameraFrame, drawCamera, aspect);

		// Named here because the transparent node reads it and that lambda has
		// a `source` of its own - a texture, not this view.
		frameUniforms = FrameUniforms{
			matrices.ViewProjection,
			lightViewProjection,
			glm::mat4{1.0f},
		};
		lighting = LightingAt(cameraFrame.Position, 0.0f, 0.0f);

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
			glm::vec4{cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z, 1.0f};
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

		tonemapBindings = {SDL_GPUTextureSamplerBinding{pbr.SkyLit, sampler}};

		return ViewStart::Recording;
	}
	// The instance rows, the slot tables beside them, and the occlusion plan
	// that partitions the opaque head.
	//
	// **This is where a `CFrame` and a `Color3` become a `mat4` and an RGBA,
	// and it is the only place in the engine that happens.** A world produces
	// scene data; a device layout is this module's business. The conversion
	// updates stable host slots, then only changed packed rows are staged for the
	// resident device pool. Visibility and order are a separate uint stream.
	//
	// **A private phase of `Begin` rather than a node**, because nothing here
	// touches the device queue: the copy pass it stages for is submitted by
	// `RecordUploads`, which is what the `upload-instances` node calls.
	void ViewRecording::PackInstances() {
		Impl *const State = this->State;
		const core::CFrame &cameraFrame = Request.CameraFrame;
		const uint32_t uploadCount = UploadCount;
		const uint32_t sceneCount = SceneCount;
		const uint32_t cameraBase = sceneCount;
		const uint32_t ownCount = OwnCount;
		const uint32_t plainOpaque = PlainOpaque;
		const bool occlusionCulling = OcclusionCulling;
		bool &haveInstances = HaveInstances;
		Impl::SceneSlot &target = State->SlotAt(Request.TargetSlot);
		Impl::InstanceWorld &residentWorld =
			State->InstanceWorldFor(Request.Source->World, Request.Source->WorldName);
		State->ActiveInstanceWorld = &residentWorld;
		InstanceResidency &residency = residentWorld.Instances;
		residency.BeginFrame(State->FrameCounter);
		target.InstanceIndices.resize(uploadCount);

		State->SlotMesh.resize(uploadCount);
		State->SlotTexture.resize(uploadCount);
		State->SlotNormalMap.resize(uploadCount);
		State->SlotRoughnessMap.resize(uploadCount);
		State->SlotOcclusionMap.resize(uploadCount);
		State->SlotHeightMap.resize(uploadCount);
		State->SlotMetalnessMap.resize(uploadCount);
		State->SlotEmissiveMap.resize(uploadCount);
		State->SlotResample.resize(uploadCount);
		State->SlotShadowDetail.resize(uploadCount);
		State->SlotShader.resize(uploadCount);
		State->SlotTags.resize(uploadCount);
		State->SlotSeam.resize(uploadCount);
		State->SlotSeamLight.resize(uploadCount);
		State->SlotInstanceKey.resize(sceneCount);
		State->SlotInstanceCurrent.resize(sceneCount);
		State->SceneSlotOfSource.resize(ownCount);
		const bool haveOwnSources = target.InstanceSourcesReady && target.InstanceSources.size() == ownCount;
		const bool rebuildOwnSources = Request.Damage.Objects || !haveOwnSources;
		if (!haveOwnSources) {
			target.InstanceSources.resize(ownCount);
			target.InstanceSourcesReady = false;
		}

		const core::Name viewWorld = Request.Source->WorldName;
		const auto writeMetadata =
			[&](uint32_t drawSlot, const scene::DrawInstance &instance, const MeshEntry *mesh) {
				State->SlotMesh[drawSlot] = mesh;
				State->SlotTexture[drawSlot] = instance.Texture;
				State->SlotNormalMap[drawSlot] = instance.NormalMap;
				State->SlotRoughnessMap[drawSlot] = instance.RoughnessMap;
				State->SlotOcclusionMap[drawSlot] = instance.OcclusionMap;
				State->SlotHeightMap[drawSlot] = instance.HeightMap;
				State->SlotMetalnessMap[drawSlot] = instance.MetalnessMap;
				State->SlotEmissiveMap[drawSlot] = instance.EmissiveMap;
				State->SlotResample[drawSlot] = instance.Resample;
				State->SlotShadowDetail[drawSlot] = instance.Alpha != scene::AlphaMode::Opaque ||
													instance.SeamNormal.MagnitudeSquared() > 0.0f;
				State->SlotShader[drawSlot] = instance.Shader;
				State->SlotTags[drawSlot] = instance.TagMask;
				State->SlotSeam[drawSlot] = glm::vec4{
					instance.SeamNormal.X,
					instance.SeamNormal.Y,
					instance.SeamNormal.Z,
					instance.SeamOffset,
				};
				State->SlotSeamLight[drawSlot] =
					glm::vec4{instance.SeamLight.X, instance.SeamLight.Y, instance.SeamLight.Z, 0.0f};
			};
		const auto probe = [&](uint32_t drawSlot,
							   const scene::DrawInstance &instance,
							   uint32_t fallback,
							   const Impl::SceneSlot::InstanceSourceRow *cached = nullptr) {
			const MeshEntry &mesh = State->Meshes.Resolve(instance.Mesh);
			writeMetadata(drawSlot, instance, &mesh);

			InstanceKey &key = State->SlotInstanceKey[drawSlot];
			key = InstanceKey{
				instance.SourceWorld.IsValid() ? instance.SourceWorld : viewWorld,
				instance.Source,
				instance.Variant,
				instance.Source == 0 ? fallback + 1u : 0u,
			};
			uint32_t residentSlot = std::numeric_limits<uint32_t>::max();
			State->SlotInstanceCurrent[drawSlot] =
				cached != nullptr && cached->Key == key
					? residency.ProbeSlot(cached->ResidentSlot, key, instance, mesh)
					: residency.Probe(key, instance, mesh, residentSlot);
			if (cached != nullptr && cached->Key == key) {
				residentSlot = cached->ResidentSlot;
			}
			target.InstanceIndices[drawSlot] = residentSlot;
		};
		const auto rememberSource = [&](uint32_t source, uint32_t drawSlot) {
			target.InstanceSources[source] = {
				.Key = State->SlotInstanceKey[drawSlot],
				.Mesh = State->SlotMesh[drawSlot],
				.ResidentSlot = target.InstanceIndices[drawSlot],
			};
		};
		const auto finishResident =
			[&](uint32_t drawSlot, const scene::DrawInstance &instance, const MeshEntry &mesh) {
				if (State->SlotInstanceCurrent[drawSlot] != 0) {
					residency.Touch(target.InstanceIndices[drawSlot]);
					return;
				}
				target.InstanceIndices[drawSlot] = residency.UpsertSlot(
					target.InstanceIndices[drawSlot],
					State->SlotInstanceKey[drawSlot],
					ToGpu(instance, mesh),
					instance,
					mesh
				);
			};

		{
			ENGINE_PROFILE_CAT("resolve resident instances", core::ProfileCategory::Render);
			{
				ENGINE_PROFILE_CAT("resident.scene", core::ProfileCategory::Render);
				if (!rebuildOwnSources) {
					for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
						const uint32_t source = State->SceneOrder[index];
						const Impl::SceneSlot::InstanceSourceRow &cached = target.InstanceSources[source];
						writeMetadata(index, State->SceneInstances[source], cached.Mesh);
						target.InstanceIndices[index] = cached.ResidentSlot;
						State->SceneSlotOfSource[source] = index;
						residency.Touch(target.InstanceIndices[index]);
					}
				} else {
					// At 100,000 resident rows this probe and metadata pass measured 10.9 ms
					// serial in release. Sixteen thousand rows leaves far more work than the
					// job system's measured handover. Smaller scenes keep probe and finalize
					// adjacent so the parallel design adds no second traversal.
					constexpr size_t RESIDENT_GRAIN = 4096;
					constexpr size_t RESIDENT_PARALLEL_MINIMUM = 16'384;
					if (State->SceneOrder.size() < RESIDENT_PARALLEL_MINIMUM) {
						for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
							const uint32_t source = State->SceneOrder[index];
							const scene::DrawInstance &instance = State->SceneInstances[source];
							probe(
								index,
								instance,
								source,
								haveOwnSources ? &target.InstanceSources[source] : nullptr
							);
							State->SceneSlotOfSource[source] = index;
							finishResident(index, instance, *State->SlotMesh[index]);
							rememberSource(source, index);
						}
					} else {
						parallel::Jobs::For(
							State->SceneOrder.size(),
							RESIDENT_GRAIN,
							[&](size_t begin, size_t end) {
								for (size_t index = begin; index < end; index++) {
									const uint32_t source = State->SceneOrder[index];
									probe(
										static_cast<uint32_t>(index),
										State->SceneInstances[source],
										source,
										haveOwnSources ? &target.InstanceSources[source] : nullptr
									);
								}
							},
							RESIDENT_PARALLEL_MINIMUM
						);
						for (uint32_t index = 0; index < State->SceneOrder.size(); index++) {
							const uint32_t source = State->SceneOrder[index];
							State->SceneSlotOfSource[source] = index;
							finishResident(index, State->SceneInstances[source], *State->SlotMesh[index]);
							rememberSource(source, index);
						}
					}
					target.InstanceSourcesReady = true;
				}
			}
			{
				ENGINE_PROFILE_CAT("resident.foreign", core::ProfileCategory::Render);
				for (uint32_t index = ownCount; index < sceneCount; index++) {
					probe(index, State->SceneInstances[index], index);
					finishResident(index, State->SceneInstances[index], *State->SlotMesh[index]);
				}
			}
			{
				ENGINE_PROFILE_CAT("resident.camera", core::ProfileCategory::Render);
				const auto mapCameraRows = [&](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						const uint32_t source = State->DrawOrder[index];
						const uint32_t drawSlot = cameraBase + static_cast<uint32_t>(index);
						const uint32_t sceneSlot = State->SceneSlotOfSource[source];
						target.InstanceIndices[drawSlot] = target.InstanceIndices[sceneSlot];
						State->SlotMesh[drawSlot] = State->SlotMesh[sceneSlot];
						State->SlotTexture[drawSlot] = State->SlotTexture[sceneSlot];
						State->SlotNormalMap[drawSlot] = State->SlotNormalMap[sceneSlot];
						State->SlotRoughnessMap[drawSlot] = State->SlotRoughnessMap[sceneSlot];
						State->SlotOcclusionMap[drawSlot] = State->SlotOcclusionMap[sceneSlot];
						State->SlotHeightMap[drawSlot] = State->SlotHeightMap[sceneSlot];
						State->SlotMetalnessMap[drawSlot] = State->SlotMetalnessMap[sceneSlot];
						State->SlotEmissiveMap[drawSlot] = State->SlotEmissiveMap[sceneSlot];
						State->SlotResample[drawSlot] = State->SlotResample[sceneSlot];
						State->SlotShadowDetail[drawSlot] = State->SlotShadowDetail[sceneSlot];
						State->SlotShader[drawSlot] = State->SlotShader[sceneSlot];
						State->SlotTags[drawSlot] = State->SlotTags[sceneSlot];
						State->SlotSeam[drawSlot] = State->SlotSeam[sceneSlot];
						State->SlotSeamLight[drawSlot] = State->SlotSeamLight[sceneSlot];
					}
				};
				constexpr size_t CAMERA_ROWS_GRAIN = 4096;
				constexpr size_t CAMERA_ROWS_PARALLEL_MINIMUM = 16'384;
				parallel::Jobs::For(
					State->DrawOrder.size(), CAMERA_ROWS_GRAIN, mapCameraRows, CAMERA_ROWS_PARALLEL_MINIMUM
				);
			}
		}

		// --- the occlusion plan --------------------------------
		//
		// Each opaque slot-run is partitioned in place: the
		// instances big and near enough to occlude move to its
		// head and draw in the early phase; the tail waits on the
		// GPU test against the pyramid the early phase's depth
		// seeds. **Only resident-slot indices move.** Every per-slot
		// array is constant across a run by `SlotsShareRun`'s
		// definition, so swapping rows inside one changes no run
		// boundary and no other pass's picture - an opaque draw
		// is order-independent under the depth test.
		State->OcclusionFrame = Impl::OcclusionPlan{};
		if (occlusionCulling && plainOpaque > 0) {
			ENGINE_PROFILE_CAT("occlusion plan", core::ProfileCategory::Render);
			Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;

			// Drawn as an occluder when its widest world extent
			// subtends at least this fraction of its distance -
			// roughly six degrees. Lower drafts more occluders and
			// costs early-phase overdraw; higher seeds the pyramid
			// with too little to cull against. Not measured
			// finely; revisit with a real scene if the cull rate
			// disappoints.
			constexpr float OCCLUDER_SCORE = 0.1f;

			const glm::vec3 eye{cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z};
			const auto base = static_cast<uint32_t>(cameraBase);
			const uint32_t opaqueEnd = base + plainOpaque;

			std::vector<uint32_t> earlyRows;
			std::vector<uint32_t> lateRows;
			uint32_t slot = base;
			while (slot < opaqueEnd) {
				uint32_t run = 1;
				while (slot + run < opaqueEnd && State->SlotsShareRun(slot, slot + run)) {
					run++;
				}

				const MeshEntry &runMesh = *State->SlotMesh[slot];
				const glm::vec3 meshCentre{runMesh.Centre.X, runMesh.Centre.Y, runMesh.Centre.Z};
				const glm::vec3 meshExtent{runMesh.Extent.X, runMesh.Extent.Y, runMesh.Extent.Z};

				const auto runIndex = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
				earlyRows.clear();
				lateRows.clear();
				for (uint32_t at = slot; at < slot + run; at++) {
					const uint32_t residentSlot = target.InstanceIndices[at];
					const GpuInstance &row = residency.Row(residentSlot);

					// The world box, from the same matrix the
					// vertex shader draws with: the mesh's own box
					// mapped through the model. `ToGpu` built the
					// model to fill the part's box exactly, so
					// this bound is tight rather than approximate.
					//
					// **Rebuilt from the packed row rather than
					// carried on it.** The row holds a rotation, a
					// scale and a translation since v0.19, and
					// `ModelMatrixOf` decodes them exactly as
					// `instance.glsl` does - so the box still
					// bounds the geometry that is drawn rather than
					// the geometry that was asked for, which is a
					// quantisation error apart.
					const glm::mat4 model = ModelMatrixOf(row);
					const glm::mat3 basis{model};
					const glm::vec3 centre = glm::vec3(model * glm::vec4(meshCentre, 1.0f));
					const glm::vec3 extent = glm::abs(basis[0]) * meshExtent.x +
											 glm::abs(basis[1]) * meshExtent.y +
											 glm::abs(basis[2]) * meshExtent.z;

					const float widest = std::max(extent.x, std::max(extent.y, extent.z));
					const float away = std::max(glm::distance(centre, eye), 0.01f);
					if (widest >= away * OCCLUDER_SCORE) {
						earlyRows.push_back(residentSlot);
					} else {
						lateRows.push_back(residentSlot);
						occlusionPlan.CandidatePairs.emplace_back(centre, std::bit_cast<float>(runIndex));
						occlusionPlan.CandidatePairs.emplace_back(extent, std::bit_cast<float>(residentSlot));
					}
				}

				const auto earlyCount = static_cast<uint32_t>(earlyRows.size());
				std::copy(earlyRows.begin(), earlyRows.end(), target.InstanceIndices.begin() + slot);
				std::copy(
					lateRows.begin(), lateRows.end(), target.InstanceIndices.begin() + slot + earlyCount
				);

				occlusionPlan.RunFirstSlot.push_back(slot);
				occlusionPlan.RunEarly.push_back(earlyCount);
				occlusionPlan.RunCandidates.push_back(static_cast<uint32_t>(lateRows.size()));
				occlusionPlan.EarlyTotal += earlyCount;
				occlusionPlan.ArgCount += DrawArgumentCount(runMesh);
				slot += run;
			}

			occlusionPlan.RunCount = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
			occlusionPlan.CandidateCount = static_cast<uint32_t>(occlusionPlan.CandidatePairs.size() / 2);

			// Nothing big enough to seed the pyramid means nothing
			// can be culled, and nothing to test means nothing to
			// cull either way - both fall back to the plain draw,
			// which is what "conservative" costs in the worst case.
			occlusionPlan.Active = occlusionPlan.EarlyTotal > 0 && occlusionPlan.CandidateCount > 0 &&
								   occlusionPlan.ArgCount > 0;
		}
		residency.EndFrame();

		if (uploadCount == 0) {
			target.ResidentIndices.Plan(
				static_cast<uint32_t>(State->FrameCounter % IndexResidency::VERSIONS), {}, false
			);
			target.ResidentIndices.Acknowledge();
			Result.InstanceChunks = 0;
			Result.InstanceChunksDirty = 0;
			Result.InstanceRows = 0;
			Result.InstanceRowsDirty = 0;
			return;
		}

		target.SkinOffsets.assign(std::max<uint32_t>(residency.SlotCount(), 1), UINT32_MAX);
		const auto assignSkin = [&](uint32_t drawSlot, const scene::DrawInstance &instance) {
			const uint32_t residentSlot = target.InstanceIndices[drawSlot];
			const MeshEntry *mesh = State->SlotMesh[drawSlot];
			const uint64_t end = static_cast<uint64_t>(instance.SkinFirst) + instance.SkinCount;
			if (residentSlot < target.SkinOffsets.size() && mesh != nullptr && instance.SkinCount != 0 &&
				mesh->JointCount == instance.SkinCount && end <= State->SceneJointFrames.size()) {
				target.SkinOffsets[residentSlot] = instance.SkinFirst;
			}
		};
		for (uint32_t drawSlot = 0; drawSlot < ownCount; drawSlot++) {
			assignSkin(drawSlot, State->SceneInstances[State->SceneOrder[drawSlot]]);
		}
		for (uint32_t drawSlot = ownCount; drawSlot < sceneCount; drawSlot++) {
			assignSkin(drawSlot, State->SceneInstances[drawSlot]);
		}
		uint64_t skinOffsetSignature = scene::MixSignature(1, target.SkinOffsets.size());
		for (const uint32_t offset : target.SkinOffsets) {
			skinOffsetSignature = scene::MixSignature(skinOffsetSignature, offset);
		}
		target.SkinOffsetsDirty = target.SkinOffsetsDirty || !target.SkinOffsetsReady ||
								  target.SkinOffsetSignature != skinOffsetSignature;
		target.SkinOffsetSignature = skinOffsetSignature;
		target.SkinOffsetsReady = true;

		target.JointWords.assign(std::max<size_t>(State->SceneJointFrames.size() * 5, 5), 0);
		for (size_t index = 0; index < State->SceneJointFrames.size(); index++) {
			const core::CFrame &frame = State->SceneJointFrames[index];
			const PackedRotation rotation = PackRotation(frame.Rotation());
			const size_t word = index * 5;
			target.JointWords[word] = std::bit_cast<uint32_t>(frame.Position.X);
			target.JointWords[word + 1] = std::bit_cast<uint32_t>(frame.Position.Y);
			target.JointWords[word + 2] = std::bit_cast<uint32_t>(frame.Position.Z);
			target.JointWords[word + 3] = rotation.Words[0];
			target.JointWords[word + 4] = rotation.Words[1];
		}
		uint64_t jointWordSignature = scene::MixSignature(1, target.JointWords.size());
		for (const uint32_t word : target.JointWords) {
			jointWordSignature = scene::MixSignature(jointWordSignature, word);
		}
		target.JointWordsDirty = target.JointWordsDirty || !target.JointWordsReady ||
								 target.JointWordSignature != jointWordSignature;
		target.JointWordSignature = jointWordSignature;
		target.JointWordsReady = true;

		const auto ensureSkinBuffer = [&](SDL_GPUBuffer *&buffer,
										  SDL_GPUTransferBuffer *&transfer,
										  uint32_t &capacity,
										  uint32_t words,
										  const char *label,
										  bool &dirty) {
			if (buffer != nullptr && transfer != nullptr && capacity >= words) {
				return true;
			}
			uint32_t grown = capacity == 0 ? 256 : capacity;
			while (grown < words) {
				grown *= 2;
			}
			if (buffer != nullptr) {
				gpu::ReleaseBuffer(State->Device, buffer);
			}
			if (transfer != nullptr) {
				gpu::ReleaseTransferBuffer(State->Device, transfer);
			}
			SDL_GPUBufferCreateInfo bufferInfo{};
			bufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			bufferInfo.size = grown * sizeof(uint32_t);
			buffer = gpu::CreateBuffer(State->Device, &bufferInfo);
			SDL_GPUTransferBufferCreateInfo transferInfo{};
			transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transferInfo.size = bufferInfo.size;
			transfer = gpu::CreateTransferBuffer(State->Device, &transferInfo);
			if (buffer == nullptr || transfer == nullptr) {
				ENGINE_ERROR("{} buffer of {} words: {}", label, grown, SDL_GetError());
				capacity = 0;
				return false;
			}
			capacity = grown;
			dirty = true;
			return true;
		};
		if (!ensureSkinBuffer(
				target.SkinOffsetBuffer,
				target.SkinOffsetTransfer,
				target.SkinOffsetCapacity,
				static_cast<uint32_t>(target.SkinOffsets.size()),
				"skin offset",
				target.SkinOffsetsDirty
			) ||
			!ensureSkinBuffer(
				target.JointBuffer,
				target.JointTransfer,
				target.JointWordCapacity,
				static_cast<uint32_t>(target.JointWords.size()),
				"joint palette",
				target.JointWordsDirty
			)) {
			return;
		}

		if (target.SkinOffsetsDirty) {
			void *skinMapped = SDL_MapGPUTransferBuffer(State->Device, target.SkinOffsetTransfer, true);
			if (skinMapped == nullptr) {
				ENGINE_ERROR("skin offsets: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
				return;
			}
			std::memcpy(skinMapped, target.SkinOffsets.data(), target.SkinOffsets.size() * sizeof(uint32_t));
			SDL_UnmapGPUTransferBuffer(State->Device, target.SkinOffsetTransfer);
		}
		if (target.JointWordsDirty) {
			void *jointMapped = SDL_MapGPUTransferBuffer(State->Device, target.JointTransfer, true);
			if (jointMapped == nullptr) {
				ENGINE_ERROR("joint palettes: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
				return;
			}
			std::memcpy(jointMapped, target.JointWords.data(), target.JointWords.size() * sizeof(uint32_t));
			SDL_UnmapGPUTransferBuffer(State->Device, target.JointTransfer);
		}
		State->SkinOffsetBuffer = target.SkinOffsetBuffer;
		State->SkinOffsetTransfer = target.SkinOffsetTransfer;
		State->JointBuffer = target.JointBuffer;
		State->JointTransfer = target.JointTransfer;

		bool rowsReallocated = false;
		bool indicesReallocated = false;
		{
			ENGINE_PROFILE_CAT("ensure instance capacity", core::ProfileCategory::Render);
			if (!State->EnsureInstanceCapacity(
					residency.SlotCount(), uploadCount, rowsReallocated, indicesReallocated
				)) {
				return;
			}
			if (rowsReallocated) {
				residency.MarkAllDirty();
			}

			constexpr uint32_t METRIC_CHUNK_ROWS = 256;
			const uint32_t residentChunks =
				(residency.SlotCount() + METRIC_CHUNK_ROWS - 1) / METRIC_CHUNK_ROWS;
			if (residentWorld.MetricFrame != State->FrameCounter) {
				Result.InstanceChunks = residentChunks;
				Result.InstanceRows = residency.LiveCount();
				residentWorld.MetricFrame = State->FrameCounter;
			}
			std::vector<bool> dirtyChunks(residentChunks, false);
			for (const InstanceUploadRange &range : residency.DirtyRanges()) {
				const uint32_t last = range.First + range.Count - 1;
				for (uint32_t chunk = range.First / METRIC_CHUNK_ROWS; chunk <= last / METRIC_CHUNK_ROWS;
					 chunk++) {
					dirtyChunks[chunk] = true;
				}
			}
			Result.InstanceChunksDirty =
				static_cast<uint32_t>(std::count(dirtyChunks.begin(), dirtyChunks.end(), true));
			Result.InstanceRowsDirty = residency.DirtyCount();
			target.ResidentIndices.Plan(
				static_cast<uint32_t>(State->FrameCounter % IndexResidency::VERSIONS),
				target.InstanceIndices,
				indicesReallocated
			);

			if (target.ResidentIndices.DirtyCount() > 0) {
				ENGINE_PROFILE_CAT("stage instance indices", core::ProfileCategory::Render);
				void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->InstanceIndexTransfer, true);
				if (mapped == nullptr) {
					ENGINE_ERROR("instance indices: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
					return;
				}
				auto *staged = static_cast<uint32_t *>(mapped);
				StageBulk<uint32_t>(staged, target.ResidentIndices.DirtyRanges(), target.InstanceIndices);
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceIndexTransfer);
			} else {
				target.ResidentIndices.Acknowledge();
			}

			if (residency.DirtyCount() > 0) {
				ENGINE_PROFILE_CAT("stage resident instances", core::ProfileCategory::Render);
				void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->InstanceTransfer, true);
				if (mapped == nullptr) {
					ENGINE_ERROR("resident instances: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
					return;
				}
				auto *rows = static_cast<GpuInstance *>(mapped);
				const std::span<const GpuInstance> packed = residency.PackedRows();
				StageBulk<GpuInstance>(rows, residency.DirtyRanges(), packed);
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceTransfer);
			}
			haveInstances = true;

			// Stage what the cull reads and the indirect draws consume. Its
			// own transfer rather than a tail on the instance one, so the
			// plain path pays nothing for a feature it never authored.
			if (State->OcclusionFrame.Active) {
				Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;
				if (!State->EnsureOcclusionResources(
						occlusionPlan.ArgCount,
						occlusionPlan.CandidateCount,
						occlusionPlan.RunCount,
						plainOpaque
					)) {
					occlusionPlan.Active = false;
				} else {
					void *staged = SDL_MapGPUTransferBuffer(State->Device, State->Occlusion.Transfer, true);
					if (staged == nullptr) {
						ENGINE_ERROR("occlusion staging: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
						occlusionPlan.Active = false;
					} else {
						const auto cameraBase = static_cast<uint32_t>(sceneCount);
						auto *commands = static_cast<SDL_GPUIndexedIndirectDrawCommand *>(staged);
						auto *lateArgRuns = reinterpret_cast<uint32_t *>(
							reinterpret_cast<uint8_t *>(staged) +
							static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
								sizeof(SDL_GPUIndexedIndirectDrawCommand) +
							static_cast<size_t>(occlusionPlan.CandidateCount) * 2 * sizeof(glm::vec4) +
							static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
						);

						// The early and late commands for one draw differ
						// only in what fills their instance count and where
						// their instances start: the early phase draws the
						// run's occluder head out of the instance buffer,
						// the late phase draws the compacted survivors out
						// of the late buffer, whose slots drop `cameraBase`
						// so a view's opaque head starts it at zero.
						uint32_t argument = 0;
						for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
							const MeshEntry &runMesh = *State->SlotMesh[occlusionPlan.RunFirstSlot[runIndex]];
							const uint32_t firstSlot = occlusionPlan.RunFirstSlot[runIndex];
							const uint32_t lateFirst =
								firstSlot - cameraBase + occlusionPlan.RunEarly[runIndex];
							const auto emitArgs = [&](const MeshRange &range) {
								if (range.IndexCount == 0) {
									return;
								}
								commands[argument] = SDL_GPUIndexedIndirectDrawCommand{
									range.IndexCount,
									occlusionPlan.RunEarly[runIndex],
									range.FirstIndex,
									range.VertexOffset,
									firstSlot,
								};
								commands[occlusionPlan.ArgCount + argument] =
									SDL_GPUIndexedIndirectDrawCommand{
										range.IndexCount,
										0,
										range.FirstIndex,
										range.VertexOffset,
										lateFirst,
									};
								lateArgRuns[argument] = runIndex;
								argument++;
							};
							if (runMesh.Runs.empty()) {
								emitArgs(runMesh.Whole);
							} else {
								for (const MeshRange &range : runMesh.Runs) {
									emitArgs(range);
								}
							}
						}

						auto *cursor = reinterpret_cast<uint8_t *>(staged) +
									   static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
										   sizeof(SDL_GPUIndexedIndirectDrawCommand);
						std::memcpy(
							cursor,
							occlusionPlan.CandidatePairs.data(),
							occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4)
						);
						cursor += occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4);

						auto *runTable = reinterpret_cast<uint32_t *>(cursor);
						for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
							runTable[runIndex] = occlusionPlan.RunFirstSlot[runIndex] - cameraBase +
												 occlusionPlan.RunEarly[runIndex];
						}
						cursor += static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t) +
								  static_cast<size_t>(occlusionPlan.ArgCount) * sizeof(uint32_t);

						// The zeros the cull's atomics count up from.
						std::memset(
							cursor, 0, static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
						);

						SDL_UnmapGPUTransferBuffer(State->Device, State->Occlusion.Transfer);
					}
				}
			}
		}
	}

	void ViewRecording::Finish(const NodeTable &frameNodes) {
		Impl *const State = this->State;
		FrameResult &result = Result;
		const Impl::NamedPipeline *const selectedPipeline = Pipeline;
		SDL_GPUCommandBuffer *const command = Command;
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

		// **The whole of the frame's recording, and it was the widest blank of
		// the lot.** Everything below this line is `RenderGraph::Execute`
		// walking the compiled graph and calling the handlers built above, and
		// neither `GraphRunner::Run` nor most of the handlers opened a span - so
		// the gbuffer, the lighting, the tonemap and the present all landed
		// inside `Renderer::RenderView` with nothing to attribute them to.
		// `GraphRunner::Run` now names each node; this is the bar they sit under.
		ENGINE_PROFILE_CAT("execute graph", core::ProfileCategory::Render);

		NodeProfileHooks profile;
		profile.Enabled = [this](const graph::RunContext &context) { return ProfileEnabled(context); };
		profile.Begin = [this](const graph::RunContext &context) { BeginNodeProfile(context); };
		profile.End = [this](const graph::RunContext &context) { return EndNodeProfile(context); };
		GraphRunner frameRunner(frameNodes, State->ProfileTier, std::move(profile));
		bool dispatched = false;
		if (State->BatchActive) {
			dispatched = selectedPipeline->Graph.ExecuteView(
				selectedPipeline->Compiled,
				frameRunner,
				State->BatchViewIndex,
				State->BatchWorldIndex,
				State->BatchShared
			);
			if (dispatched && State->BatchFinal) {
				dispatched = selectedPipeline->Graph.ExecuteFinal(selectedPipeline->Compiled, frameRunner);
			}
		} else {
			const uint64_t worlds[] = {world};
			dispatched = selectedPipeline->Graph.Execute(selectedPipeline->Compiled, frameRunner, worlds);
		}
		State->DroppedProfileMarks += frameRunner.DroppedProfileMarks();
		if (!dispatched) {
			ENGINE_ERROR(
				"render graph '{}' refused while executing '{}'",
				selectedPipeline->Name.Text(),
				frameRunner.Unhandled().Text()
			);
			closePass();
			State->BatchFailed = State->BatchActive;
			endIncompleteView();
			return;
		}

		closePass();

		// Byte and two-byte masks are expanded to RGBA by the readback path. Float
		// targets remain directly inspectable on the GPU without pretending their
		// bytes are display colour.
		if (offscreen && State->Inspected.IsValid() &&
			(State->InspectedSlot == Renderer::ANY_VIEWPORT || State->InspectedSlot == targetSlot)) {
			Impl::NamedTexture inspected = fixedTexture(State->Inspected, targetSlot);
			if (!inspected.IsValid()) {
				for (const Impl::GraphTarget &target : State->GraphTargets) {
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

		if (captureSource.IsValid() && !capturePath.empty()) {
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
			}
		}

		// Keep the interface-facing image separate from the target being written.
		// Its fence is attached when the batch submits below, and the CPU publishes
		// it on a later frame only after that fence signals.
		if (offscreen) {
			(void)State->RetainSceneFrame(command, targetSlot, result);
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

		// Every earlier view has finished recording, but the command buffer still
		// belongs to the batch. The last view alone transfers ownership to SDL.
		if (State->BatchActive && !State->BatchFinal) {
			return;
		}
		if (State->BatchActive) {
			State->BatchCommand = nullptr;
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
			if (!State->SubmitSceneCommand(command)) {
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
				State->CompleteResidentUploads(false);
				State->Timestamps.Abandon(timingSlot);
				if (timingSlot < VulkanTimestamps::SLOTS) {
					State->PendingMarks[timingSlot].clear();
				}
				if (capture != nullptr) {
					gpu::ReleaseTransferBuffer(State->Device, capture);
				}
				State->DropDownloads();
				return;
			}
			State->CompleteResidentUploads(true);

			if (SDL_GPUCommandBuffer *downloads = State->DownloadCommand; downloads != nullptr) {
				State->DownloadCommand = nullptr;
				result.DownloadCommandBuffers++;
				if (capture != nullptr) {
					// **A fence, and the stall is the point.** The pixels are
					// not there until the GPU has run the copy, so a capture has
					// to wait for it. That is a frame's worth of latency on the
					// frames a caller asked to capture and on no others. The
					// main buffer is already on the queue, so a failure here
					// loses this frame's downloads, never the frame itself.
					SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(downloads);
					if (fence == nullptr) {
						ENGINE_ERROR("SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError());
						gpu::ReleaseTransferBuffer(State->Device, capture);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->Preview.Pixels.clear();
							State->Preview.Histogram = ImageHistogram{};
						}
					} else {
						SDL_WaitForGPUFences(State->Device, true, &fence, 1);
						SDL_ReleaseGPUFence(State->Device, fence);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->CollectPreview();
						}

						if (State->WriteCapture(
								capture,
								captureSource.Width,
								captureSource.Height,
								captureSource.Format,
								capturePath
							)) {
							ENGINE_INFO(
								"captured {} x {} to {}",
								captureSource.Width,
								captureSource.Height,
								capturePath.string()
							);
						}

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
