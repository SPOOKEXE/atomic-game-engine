#pragma once

// Client renderer. GPU types remain private to the implementation.
//
// @tier L12 · client

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Flipbook.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/render/Readback.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

struct SDL_Window;

namespace engine::render {

	// The stages `Render` submits, in submission order.
	//
	// **Read out of `graph::StandardGraph` rather than written here**, which is
	// the whole point: the frame is described once, as a graph, and this is a
	// view of it. There used to be a `Pass` enum beside it saying the same six
	// things in the same order with a test to keep the two in step — a test that
	// can tell you two descriptions disagree but not which one is right, and
	// that only ever runs after somebody has already changed one. `DEFERRED.md`
	// D00016 is the entry about what that costs.
	//
	// What it is still for is the bit positions in `FrameResult::Passes`: a
	// caller asking whether the shadow pass ran needs an index space, and this
	// is the one the graph itself defines.
	//
	// @return The six names, valid for the life of the program.
	std::span<const core::Name> PassOrder();

	// A second view, rendered into a texture instead of the swapchain.
	//
	// Surface textures are double-buffered; nested reflections are one frame stale.
	//
	// @since v0.6
	struct SurfaceView {
		// Which surface index this renders, matching `scene::Visual::Surface` on
		// whatever samples it.
		//
		// **The pairing is by number and nothing else**, which is what lets a
		// replica reflect at all: the wire carries components and not the tree,
		// so "the camera belonging to this pane" cannot be a parent link on the
		// far end. `scene::AimSurfaceCameras` is what makes the two numbers
		// agree, by writing the camera's index onto the pane it is parented to.
		//
		// At or above `scene::MAX_SURFACES` the view is dropped with a line in
		// the log, rather than silently rendering nothing.
		int8_t Index = 0;

		// Where the surface camera is, in world space.
		core::CFrame Frame;

		// Its field of view and clipping distances.
		scene::Camera Lens;

		// How wide the texture is. Square is not required; a wide mirror wants a
		// wide target, and giving it a square one wastes half the texels.
		uint32_t Width = 1024;

		// How tall the texture is.
		uint32_t Height = 1024;

		// How opaque the projected image is, 0 transparent to 1 solid.
		//
		// **The image's own opacity and not the part's**, which is the whole
		// distinction `scene::SurfaceCamera::ImageTransparency` exists to make:
		// how much of the world shows through the glass and how much of the glass
		// shows through the reflection are two facts, and writing the image with
		// the part's alpha meant fading a mirror faded its reflection to nothing.
		//
		// Carried as opacity rather than as transparency because that is what the
		// shader multiplies by; the flip happens once, where the component is
		// read, rather than in a shader nobody can put a breakpoint in.
		float ImageOpacity = 1.0f;

		// Which tags an instance must carry to be drawn into this surface, or
		// zero for all of them.
		//
		// From `scene::SurfaceCamera::TagFilter`. **Applied per instance in the
		// draw loop rather than by re-ordering the draw list**, because the
		// order is shared by every view and a filter is per view: partitioning
		// it for one surface would be partitioning it for all of them, and the
		// screen pass would then draw the group instead of the world.
		uint32_t TagFilter = 0;
	};

	// How many local lights one frame may carry.
	//
	// **This is the one home of the number, and `opaque.frag` is told it.**
	// `mono.engine/render/CMakeLists.txt` reads the value out of this
	// declaration and passes `-DMAX_LIGHTS` to glslc, so the shader's loop bound
	// and the uniform buffer this sizes cannot disagree — there is no second
	// literal left to drift. Changing it here changes both.
	//
	// It was spelled twice until v0.10 and nothing checked it, which `AGENTS.md`
	// rule 6 calls documentation rather than a constraint. The reason it is
	// arranged so a mismatch is impossible, rather than tested for, is that it
	// could not be tested for: what gets staged is SPIR-V, and the constant is
	// folded away by then.
	//
	// A mismatch was never a validation error. It is a light set that silently
	// reads past its own count or stops short of it — which looks like one lamp
	// not working.
	//
	// **Keep this a plain integer literal on one line.** The configure step
	// matches it with a regex and fails loudly if it cannot, so an expression
	// here is a configure error rather than a silent revert to a stale value.
	//
	// @since v0.10
	inline constexpr size_t MAX_SCENE_LIGHTS = 16;

	// One local light, resolved into world space.
	//
	// **Resolved rather than an entity, for `SurfaceView`'s reason**: this is what
	// the device layer takes, and a world's identifier in it would be a world's
	// business leaking into a pipeline. `scene::Light` says what a light *is*;
	// where it shines from is its parent's, and the client walks that.
	//
	// @since v0.10
	struct SceneLight {
		// Where it is, in world space.
		core::Vector3 Position;

		// How far it reaches, in metres. Past this it contributes nothing, which
		// is what lets a fragment reject it with one compare.
		float Range = 8.0f;

		// Its colour, already multiplied by brightness.
		//
		// **Multiplied here rather than in the shader**, because brightness is a
		// scalar an author sets and the shader wants a colour — folding them at
		// the boundary is one multiply per light per frame against one per light
		// per *fragment*.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};

		// Which way a spot points. Ignored when `ConeCosine` is -1.
		core::Vector3 Direction{0.0f, -1.0f, 0.0f};

		// The cosine of the cone's half-angle, or -1 for a point light.
		//
		// **A cosine and not an angle**, because the test is a dot product: an
		// angle would be an `acos` per light per fragment to compare something the
		// dot product already gives.
		float ConeCosine = -1.0f;
	};

	// One emitter's worth of particles, and the state they share.
	//
	// **A batch rather than a per-particle description, and that is the whole of
	// why `effects::ParticleInstance` is twenty-eight bytes.** Texture, blend mode
	// and flipbook layout are the same for every particle of one emitter, so they
	// travel once per emitter and the particles carry nothing but what varies.
	// Half a million particles times the four bytes a texture name would have cost
	// is two megabytes a frame.
	//
	// **A span and not a copy.** The pool is the caller's and the renderer reads
	// it during the call; nothing is retained past `Render`, exactly as the draw
	// list is not.
	//
	// @since v0.10
	struct ParticleBatch {
		// This emitter's live particles, contiguous.
		//
		// The pool's blocks are contiguous per emitter with the live ones a
		// prefix — `effects::ParticleSystem` — so a batch is that prefix and
		// nothing has to be copied to produce one.
		std::span<const effects::ParticleInstance> Particles;

		// Which texture, by name. Invalid draws an untextured quad, which is a
		// visible flat square rather than nothing.
		core::Name Texture;

		// How many cells the flipbook has on each side. One is not a flipbook.
		//
		// **A side rather than a layout enum**, because that is what the shader
		// divides by — converting an enum to a side in the renderer would be
		// doing per emitter what `effects::FlipbookSide` already does at compile
		// time on the other side of the boundary.
		float FlipbookSide = 1.0f;

		// How far towards the eye the quads are nudged, in metres.
		float ZOffset = 0.0f;

		// Whether the colour is added to the target rather than blended into it.
		//
		// **Selects a pipeline and not a uniform**, because blend state is baked
		// into a pipeline. Batches are drawn blended-first then additive, so the
		// pipeline is bound twice per frame rather than once per emitter.
		bool Additive = false;

		// Whether the quad keeps world up rather than the camera's.
		//
		// From `effects::ParticleOrientation::FacingCameraWorldUp`, which is what
		// stops a column of smoke rolling when the camera does.
		bool WorldUp = false;

		// Explicit padding, for the reason every `Reserved` in the engine exists.
		uint8_t Reserved[2] = {};
	};

	// An offscreen colour target the world is drawn into instead of the window.
	//
	// **The editor's requirement, and the transparent-window trick it replaced
	// could not work.** A studio wants the world inside a dockable panel, and
	// the obvious cheap answer — draw the world to the swapchain and put a
	// background-less imgui window over it — fails the moment that window is
	// actually docked. `imgui.cpp`'s `central_node_hole` is only punched while
	// the central node is *empty*, so docking a panel into it makes the
	// dockspace fill its whole rectangle with `ImGuiCol_WindowBg` and paint over
	// the frame. The hole is a hole, not a panel.
	//
	// So the world goes into a texture and the panel shows it. That is what
	// every editor does, and it buys more than the fix: the viewport becomes an
	// ordinary window that docks, floats, resizes and closes like every other,
	// and several views become a matter of several targets rather than a second
	// arrangement of the window.
	//
	// The texture is the swapchain's format, because the pipelines that draw
	// into it were built against that format, and it is sampleable, because
	// something has to show it.
	//
	// @since v0.7
	struct SceneTarget {
		// How wide the texture is, in pixels. Zero draws to the window instead.
		uint32_t Width = 0;

		// How tall the texture is, in pixels. Zero draws to the window instead.
		uint32_t Height = 0;

		// Reports whether this asks for an offscreen target at all.
		//
		// @return `true` when both dimensions are positive.
		bool IsValid() const {
			return Width > 0 && Height > 0;
		}
	};

	// How much of a slot's texture the world was actually drawn into.
	//
	// **The texture is bigger than the panel on purpose, and this is how a
	// caller finds the part that is the picture.** A target allocated to the
	// panel's exact size has to be destroyed and created again on every frame
	// of a drag — and worse than the allocation, the *new* texture is not the
	// one the interface already recorded a bind of, so the panel spends the
	// whole drag showing the previous frame's image stretched to a rectangle it
	// was never drawn for. Rounding the allocation up to a block keeps one
	// texture alive across the whole drag, which means the image the panel
	// samples is the one this frame drew.
	//
	// What that costs is a border of pixels nothing draws into, and what it
	// needs is for whoever shows the texture to sample only the corner that is
	// the world. These are those coordinates: `(0, 0)` to `(U, V)`.
	//
	// @since v0.7
	struct SceneExtent {
		// The right edge of the drawn region, as a fraction of the texture.
		float U = 1.0f;

		// The bottom edge of the drawn region, as a fraction of the texture.
		float V = 1.0f;
	};

	// The backend handles a hook needs to build its own pipelines.
	//
	// **Opaque on purpose.** `Device` is an `SDL_GPUDevice *` and `ColourFormat`
	// is an `SDL_GPUTextureFormat`, and neither name appears here — this header
	// says it holds no SDL GPU type and that sentence is load-bearing. A hook
	// casts them back, which is a cast a caller writes once in a file that
	// already includes SDL, rather than SDL appearing on the include line of
	// everything that draws.
	//
	// @since v0.7
	struct BackendHandles {
		// The GPU device, as an `SDL_GPUDevice *`. Null before Initialise.
		void *Device = nullptr;

		// The swapchain's colour format, as an `SDL_GPUTextureFormat`.
		//
		// A pipeline built against the wrong one is a validation error at
		// creation on some drivers and a corrupt image on others, which is why
		// it is handed over rather than guessed.
		uint32_t ColourFormat = 0;
	};

	// A layer that records into this renderer's frame.
	//
	// **This exists so that Dear ImGui is not in the engine.** An editor needs
	// its chrome inside the same command buffer as the world — SDL's GPU API
	// acquires a swapchain texture once per command buffer, so a second pass in
	// a second buffer is not an option — and the two ways to arrange that are
	// both worse than this one. Putting imgui in `render` puts it on every
	// shipping client's link line to draw nothing. Exposing `SDL_GPUCommandBuffer`
	// on `Render` breaks the rule at the top of this file.
	//
	// So the renderer hands over its command buffer and render pass as `void *`,
	// and `mono.engine/ui` is the only module in the repository that knows what
	// Dear ImGui is. The lost type safety is real and is the price; it is paid
	// at exactly two call sites, both inside one file.
	//
	// **`Prepare` is outside every render pass and `Record` is inside the last
	// one.** That split is not stylistic: uploading vertices is a copy pass, and
	// a copy pass cannot be started while a render pass is open. A hook that
	// uploaded from `Record` would work until the first frame with enough
	// widgets to grow its buffer.
	//
	// @since v0.7
	// @client
	class FrameOverlayHook {
	  public:
		virtual ~FrameOverlayHook() = default;

		// Uploads whatever this frame needs, before any render pass is open.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @return `false` to skip `Record` — nothing to draw, which is not an
		//         error and not a reason to fail the frame.
		virtual bool Prepare(void *commandBuffer) = 0;

		// Records draw commands into the swapchain.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @param renderPass    An open `SDL_GPURenderPass *` bound to the
		//                      swapchain with no depth attachment.
		virtual void Record(void *commandBuffer, void *renderPass) = 0;
	};

	// One view of one world, and everything that is that view's alone.
	//
	// **The twelve parameters `Render` used to take, minus the two that were
	// never the view's.** Four of those twelve arrived after v0.9, each by the
	// same route — a feature needed one more span, and the shortest change was
	// one more trailing default argument. `DEFERRED.md` D00002 recorded where
	// that ends: *"handle multiple worlds in parallel is the first roadmap line
	// that cannot be met by making the function longer."*
	//
	// **What makes this a view rather than a rectangle**, which is the mistake
	// v0.7 made and deleted inside one version. `render::Viewport` described
	// *where a view lands*; every field here is about *what is seen* — the eye,
	// the lens, the world, the lights near it. The test the deleted type failed
	// is whether `graph::Node::PerView` can be read against it, and it can: a
	// shadow map is a function of the world and the sun, so it is shared across
	// these, and a colour pass is a function of `CameraFrame` and is not.
	//
	// The overlay and the interface hook are deliberately *not* here. Both are
	// the window's rather than a view's — one CPU image and one editor's chrome,
	// drawn once over whatever the views produced — so they stay parameters of
	// the frame. See `Renderer::Render`.
	//
	// Nothing here is retained past the call: the spans are read during it and
	// the values are copied.
	//
	// @since v0.11
	// @client
	struct View {
		// World-space placement of the camera.
		core::CFrame CameraFrame;

		// Field of view and clipping distances.
		//
		// No aspect ratio, because it is the target's rather than the author's —
		// `scene::ResolveCamera` takes one so a frame caught during a resize is
		// projected for the image it is actually drawn into.
		scene::Camera Camera;

		// What to draw, as the world described it.
		//
		// **Per view rather than per frame, which is what "multiple worlds"
		// means.** Two views of one world pass the same span; a studio showing a
		// server's world beside a client's passes two different ones, and
		// nothing in the renderer needs to know which case it is in.
		std::span<const scene::DrawInstance> Instances;

		// The offscreen views to render first, one per surface index. Empty for
		// a scene with no mirror in it.
		std::span<const SurfaceView> Surfaces;

		// Draw the world into an offscreen texture of this size instead of into
		// the window, or null for the window. Decides the aspect ratio and the
		// cull frustum as well as the target — see `SceneTarget`.
		const SceneTarget *Target = nullptr;

		// Which viewport this is, selecting both the scene texture and the
		// surface texture bank. A program with one view uses 0 and never sets
		// this. See `Renderer::SceneTexture`.
		size_t Slot = 0;

		// Which world this view is of.
		//
		// **An opaque number, and deliberately not a `world::WorldId`.** `render`
		// is L12 `client` and a world's identifier is L4's; what this needs is
		// only whether two views are of the same world, so it takes whatever the
		// caller already uses to tell them apart and never learns what a world
		// is. `world::WorldId::Value` is the obvious thing to pass.
		//
		// **What it decides is how much shared work a frame does.** A shadow map
		// is per world per light, so views sharing a world share one and views of
		// different worlds need one each — see `graph::RenderGraph::Execute`.
		// Leaving it at zero puts every view in one world, which is what a game
		// and a single-panel editor both are.
		uint64_t World = 0;

		// Which pipeline this camera's frame is described by.
		//
		// **A camera does not have to draw the way another one does.** A
		// reflection can run a cheaper chain, a debug viewport a different one,
		// and a world's main view its own — which is what
		// `graph::PipelineSet` was built to hold and what nothing could
		// previously select from. Hand `Renderer::SetPipeline` a name and put
		// that name here.
		//
		// **Unset means the renderer's default**, which is the standard frame
		// unless something replaced it. A caller that never names one behaves
		// exactly as it did before pipelines were selectable.
		//
		// @since v0.11
		core::Name Pipeline;

		// One batch per emitter with live particles, drawn after the blended
		// geometry and before the overlay.
		//
		// **After the blended pass and never sorted against it**, which is the
		// same trade `scene::ScenePlan::TransparentSurfaces` makes for mirrors:
		// a particle in front of a pane of glass is drawn after it whatever the
		// depths say. One sorted run per pipeline is what lets the blend mode be
		// a pipeline binding instead of a per-fragment branch, and interleaving
		// half a million particles into the geometry sort would cost more than
		// the artefact does.
		std::span<const ParticleBatch> Particles;

		// Every beam and trail vertex this world produced, as
		// `effects::BuildRibbons` packed them. The whole stream rather than one
		// run, because the runs index into it.
		std::span<const effects::RibbonVertex> RibbonVertices;

		// Where each ribbon sits in that stream.
		//
		// **Drawn after the particles**, which is one more fixed ordering in a
		// pass that has several. A beam is usually the thing an author wants on
		// top of its own sparks, so the order is the useful one rather than an
		// accident.
		std::span<const effects::RibbonRun> RibbonRuns;

		// The point and spot lights near this view, at most `MAX_SCENE_LIGHTS`.
		//
		// **Added to the directional term rather than replacing it**, so a scene
		// with no lamps looks exactly as it did before v0.10.
		//
		// Anything past the cap is dropped, and the caller is the one that
		// should be choosing which: the renderer has no idea which lamp matters.
		// `client::CollectLights` picks the nearest to the eye.
		std::span<const SceneLight> Lights;
	};

	// What one `Render` call submitted, and whether it reached the display.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @since v0.1
	// @client
	struct FrameResult {
		// Whether SDL accepted a command buffer for presentation.
		bool Presented = false;

		// Number of opaque and overlay draw calls submitted for this frame.
		uint32_t DrawCalls = 0;

		// Number of opaque mesh triangles submitted for this frame.
		uint64_t Triangles = 0;

		// How many instances showed a surface texture.
		uint32_t SurfaceInstances = 0;

		// How many surface cameras re-rendered this frame.
		//
		// **Not how many exist**, which is the whole reason it is worth
		// reporting. A surface whose scene has not changed keeps the texture it
		// has and runs no pass, so a room with four mirrors in it costs four
		// passes on the frames something moves and none on the frames nothing
		// does. Those two frames are indistinguishable from the draw-call count
		// — a skipped surface pass and one that ran and changed nothing look
		// identical from there — and this is the number that tells them apart.
		//
		// Zero with mirrors on screen is the ordinary case for a still scene and
		// is not a failure. Equal to the mirror count on every frame means the
		// signature is never matching, which usually means something in the draw
		// list is moving that nobody thinks is moving.
		//
		// @since v0.8
		uint32_t SurfacePasses = 0;

		// How many ribbon vertices were submitted this frame.
		//
		// Two per segment, so a beam is twenty-two and a trail is at most
		// thirty-two. Reported beside the particle count because the two are the
		// same question asked of the other half of the module.
		//
		// @since v0.10
		uint32_t RibbonVertices = 0;

		// How many bytes crossed from the CPU into GPU memory this frame, and
		// in how many copies.
		//
		// **The traffic nobody could see.** A frame's cost is not only what the
		// GPU draws; it is also what the CPU had to hand it first — the instance
		// buffer rebuilt every frame, the particles, the ribbons, and every mesh
		// or texture a streaming world uploaded while somebody walked into a new
		// area. `docs/PIPELINE_NODES.md` §7 asks for exactly this, and the ask
		// was worded as *"visibly see what goes between the CPU and the GPU,
		// what images are uploaded"*.
		//
		// **Counted at the copy, not estimated from the counts.** A version
		// derived from the instance count would be a number that agreed with the
		// truth until the layout changed, which is the sort of statistic that is
		// worse than none.
		//
		// Per-frame, and reset with the rest of the result. A steady scene
		// settles to the instance buffer alone; a spike is something arriving.
		//
		// @since v0.11
		//@{
		uint64_t UploadedBytes = 0;
		uint32_t Uploads = 0;
		//@}

		// How many particles were submitted this frame.
		//
		// **Reported because the number is the whole diagnosis for a scene that
		// is slow and looks fine.** An emitter whose rate ran away is invisible —
		// the particles are small and transparent — and shows up here as a count
		// an order of magnitude above what the scene should have.
		//
		// @since v0.10
		uint32_t Particles = 0;

		// How many instances the frustum rejected.
		//
		// **Reported rather than inferred**, because the interesting number is
		// the ratio and the denominator is the caller's draw list — which the
		// caller has and this does not need to repeat. A camera framing its own
		// scene culls almost nothing and a camera inside a large world culls
		// almost everything; a reading that never moves means the frustum is
		// wrong, not that the scene is small.
		uint32_t Culled = 0;

		// One bit per `Pass` that submitted work this frame.
		//
		// **Every pass is skippable and most of them usually are**, so "the
		// shadow pass exists" and "the shadow pass ran" are different questions,
		// and only the second one explains a frame with no shadows in it. The
		// draw-call count cannot answer it — the shadow pass and the overlay
		// pass are one draw each and look identical from there.
		//
		// @since v0.6
		uint8_t Passes = 0;

		// Whether one stage submitted work this frame.
		//
		// **Named rather than indexed**, because the stages are a graph's nodes
		// now and not an enumerator a caller can count on staying at the same
		// number. A kind the standard frame does not name is `false` rather than
		// an error: a caller asking about a pass this renderer has never had is
		// asking a fair question with a plain answer.
		//
		// @param kind Which stage — `shadow`, `surface`, `opaque`,
		//             `transparent`, `overlay` or `interface`.
		// @return True when it ran.
		bool Ran(core::Name kind) const {
			const std::span<const core::Name> order = PassOrder();
			for (size_t index = 0; index < order.size() && index < 8; index++) {
				if (order[index] == kind) {
					return (Passes & static_cast<uint8_t>(1u << index)) != 0;
				}
			}
			return false;
		}
	};

	// Owns the client GPU device, window claim, pipelines, and per-frame upload resources.
	//
	// @client
	class Renderer {
	  public:
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
		// do not run — but the world is still drawn, into the `SceneTarget` a
		// caller passes to `Render`. That is what makes a build server, a golden
		// image comparison and a scripted editor possible without a display,
		// and a headless `Render` with no scene target draws nothing rather than
		// pretending to.
		//
		// @param window SDL window to claim, or null for headless.
		// @return True when the device, pipelines and geometry are ready.
		bool Initialise(SDL_Window *window);

		// Whether this renderer has a window to present to.
		//
		// @return `true` when `Initialise` was given none.
		bool IsHeadless() const;

		// Waits for GPU work, releases the window claim and resources, and becomes uninitialised.
		//
		// Calling this on an uninitialised renderer has no effect.
		void Shutdown();

		// Registers a mesh under the name a `DrawInstance` will ask for.
		//
		// **The one door content comes in through**, and it is on the renderer
		// rather than on the table so that `MeshTable` stays an implementation
		// detail behind the pimpl like everything else here. A caller holding a
		// `delivery::Asset` reads it into an `assets::MeshData` and hands it
		// over; nothing about the device reaches them.
		//
		// Registering a name twice replaces it. The old geometry stays in the
		// buffer as dead space — nothing evicts yet, and `MeshTable`'s header
		// says so.
		//
		// @param name The name to publish it under.
		// @param mesh The geometry. An invalid one is refused.
		// @return `false` for an invalid mesh, a full table or a failed upload.
		bool AddMesh(const core::Name &name, const assets::MeshData &mesh);

		// A registered mesh's own half-extent, in mesh space.
		//
		// **So an editor can make `Size` mean the mesh's proportions.** Since
		// `Size` is a box the mesh is stretched into, a part whose box has the
		// wrong shape distorts whatever is put in it — and the only thing that
		// knows the right shape is the geometry. `render::MeshEntry::Extent`
		// carries the whole argument.
		//
		// @param name The mesh.
		// @param out  Set only when the mesh is registered.
		// @return `false` for a name this table does not hold, so a caller can
		//         tell "not loaded yet" from "flat on one axis".
		bool MeshExtentOf(const core::Name &name, core::Vector3 &out) const;

		// Registers a texture under the name a `SurfaceAppearance` or a submesh
		// will ask for.
		//
		// @param name  The name to publish it under.
		// @param image The pixels. An invalid one is refused.
		// @return `false` for an invalid image, a full table or a failed
		//         upload.
		bool AddTexture(const core::Name &name, const assets::TextureData &image);

		// How long animation has been running, for anything played on a clock.
		//
		// **The caller's clock, because this module holds none** — the rule the
		// whole engine keeps. A client passes its own accumulated seconds and so
		// does the studio; a paused editor simply stops advancing it, which is
		// what makes a paused world's GIFs hold their frame with no second
		// mechanism for it.
		//
		// @param seconds Seconds since the session began.
		// @since v0.10
		void SetAnimationTime(double seconds);

		// The backend handle for a registered texture, for an interface pass to
		// sample.
		//
		// **A `void *` for `SceneTexture`'s reason, written there in full**: the
		// header must not name an `SDL_GPUTexture`, because that would put the
		// backend's type in the interface every consumer of this header
		// compiles against. A caller that draws it already knows which backend
		// it is talking to — `ImGui::Image` takes the same opaque handle.
		//
		// **For an editor's thumbnail and not for the draw path.** The renderer
		// resolves its own textures by name inside the frame; this exists so a
		// panel can put a picture of one in a list, which is a thing only a tool
		// does.
		//
		// @param name The name it was registered under.
		// @return The handle, or nullptr for a name this renderer has not been
		//         given.
		// @since v0.10
		void *TextureHandle(const core::Name &name) const;

		// Where a texture's current animation cell sits.
		//
		// **For an interface painter, which has a name and no table.** The
		// opaque pass reads the same thing from the table directly; this is the
		// same answer for the two callers outside this module.
		//
		// @param name    The texture.
		// @param seconds How long animation has been running.
		// @return The transform, or the identity for a still or an absent name.
		// @since v0.10
		FlipbookCell TextureCell(const core::Name &name, double seconds) const;

		// How big a registered texture is, in source pixels.
		//
		// **Handed out with the handle, because an interface painter needs
		// both.** A nine-sliced or tiled `ImageLabel` is laid out in source
		// pixels — its slice insets are in them — so a resolver returning a
		// handle alone makes every slice the wrong size, which reads as a
		// corrupt image rather than as a missing measurement.
		//
		// @param name   The name.
		// @param width  Set to the width, or left alone when the name is absent.
		// @param height Set to the height, likewise.
		// @return `false` for a texture this renderer does not hold.
		// @since v0.10
		bool TextureSize(const core::Name &name, uint32_t &width, uint32_t &height) const;

		// Forgets a registered texture and frees it.
		//
		// **Because a thumbnail cache has to have a ceiling.** Every other
		// texture here is content that lives as long as the session; a preview
		// is built for a row somebody scrolled past, and a table that only ever
		// grew would hold a store's worth of images in video memory by the time
		// somebody had browsed it.
		//
		// @param name The name to drop.
		// @return `false` for a name this renderer does not hold.
		// @since v0.10
		bool DropTexture(const core::Name &name);

		// Whether a mesh has been registered under a name.
		//
		// **For a caller deciding what to fetch**, not for the draw path: an
		// unregistered mesh draws as a cube rather than failing, so the renderer
		// itself never asks.
		//
		// @param name The name.
		// @return `true` when it is registered.
		bool HasMesh(const core::Name &name) const;

		// Whether a texture has been registered under a name.
		//
		// @param name The name.
		// @return `true` when it is registered.
		bool HasTexture(const core::Name &name) const;

		// Waits for the display and claims this frame's image, before the caller
		// has read a single event.
		//
		// **Optional, and the reason to call it is latency rather than
		// throughput.** `Render` waits for the swapchain itself when this was not
		// called, so an existing loop is correct without it and measures the same.
		// What it cannot be is *responsive*: the wait is the better part of a
		// frame with vertical sync on, and a loop that pumps events and then
		// waits has already read its input by the time it starts waiting — so
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
		// @return `false` when there was nothing to acquire — minimised or
		//         mid-resize, which is not an error and not a reason to stop
		//         ticking. Headless always succeeds; it waits for nothing.
		bool WaitForFrame();

		// Reports whether the caller is the thread that owns this renderer.
		//
		// Recording is single-threaded; the owner is the thread that initialised it.
		//
		// @return `true` when the current thread may record and submit.
		// @threadsafe
		bool IsOnOwningThread() const;

		// Reports whether Initialise completed and a GPU device is available.
		bool IsInitialised() const;

		// "vulkan", "metal", "direct3d12". Shown in the F3 panel, because the
		// first question about a performance report is which backend produced
		// it.
		//
		// The returned view belongs to the renderer and is invalidated by Shutdown
		// or destruction. It is empty before successful initialisation.
		std::string_view BackendName() const;

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

		// Draws one frame from any number of views and presents it.
		//
		// **One frame, many views, and that is the whole of this signature.** Every
		// view in `views` is recorded into one command buffer, which is acquired
		// once and submitted once. Passing four views draws four viewports in a
		// frame; passing one is what a game does and costs exactly what it did.
		//
		// **What used to be twelve parameters is now `View` and two things that
		// were never a view's.** The overlay is one CPU image for the window and
		// the interface hook is the editor's chrome drawn over everything, so both
		// belong to the frame however many worlds it shows. See `View` for why the
		// rest moved and for `DEFERRED.md` D00002, which is the entry this closes.
		//
		// **An empty `views` is legal and draws no world.** It is what a frame that
		// is only an interface looks like — the window is still cleared, the
		// overlay and the hook still draw, and the frame still presents. A caller
		// with nothing to show should not have to invent a camera to say so.
		//
		// Returns `Presented = false` when the swapchain had no texture — minimised,
		// or resizing — which is not an error and not a reason to stop ticking. It
		// is also false if SDL rejects command submission.
		//
		// `overlay` is uploaded only when it has something pending, and drawn
		// whenever it has content — the texture holds the last thing sent to it,
		// so a caller may redraw the overlay far less often than it presents.
		// The renderer marks the overlay uploaded once it has recorded the copy,
		// which is the only reason it is not a const reference; nothing else
		// about it is retained past the call.
		//
		// The aspect ratio comes from the swapchain texture this call acquired
		// rather than from a caller, so a frame taken during a resize is
		// projected for the image it is actually drawn into. `scene::Camera`
		// therefore holds no aspect ratio and `scene::ResolveCamera` takes one.
		//
		// @param views    What to draw, one entry per viewport. Read during the
		//                 call and not retained. Two views naming the same `Slot`
		//                 is refused with a warning rather than drawn, because the
		//                 second would overwrite the first\'s texture and the frame
		//                 would silently show only one of them.
		// @param overlay  CPU premultiplied RGBA8 overlay. Uploaded only when it
		//                 has a pending region, drawn whenever it has content, and
		//                 marked uploaded on the way out.
		// @param interfaceHook An editor\'s chrome, drawn last, or null. See
		//                 `FrameOverlayHook` for why this is not imgui.
		// @return Submitted draw counts, summed over every view, and whether the
		//         frame was presented.
		FrameResult
		Render(std::span<const View> views, OverlayImage &overlay, FrameOverlayHook *interfaceHook = nullptr);

		// The texture the most recent `Render` drew that slot's world into.
		//
		// **Slots exist because an editor has more than one viewport.** A game
		// draws one view of one world and only ever uses slot 0. A studio
		// showing a server's world beside a client's keeps a target per panel —
		// and they must be *separate* targets, because the panels are different
		// sizes and a single shared one would be reallocated twice a frame as
		// each panel asked for its own dimensions. That reallocation is
		// measurable: it is a colour and a depth texture destroyed and created
		// per frame.
		//
		// **Valid until the next `Render` into that slot with a different
		// size**, which is when the target is reallocated. An interface layer hands it straight to
		// whatever draws it — for Dear ImGui's SDL_GPU backend that is an
		// `ImTextureID`, which is an `SDL_GPUTexture *` and therefore this
		// pointer unchanged.
		//
		// @param slot Which offscreen target to ask about. A program with one
		//             viewport uses 0 and never passes this.
		// @return The texture, or `nullptr` when nothing has been drawn into
		//         that slot.
		void *SceneTexture(size_t slot = 0) const;

		// One of the pipeline's own resources, as something a panel can draw.
		//
		// **What makes a profiler show pictures rather than numbers.** The
		// access grid says which pass wrote what; this is what turns a cell in
		// it into the image that pass produced. A `void *` for `SceneTexture`'s
		// reason, and the same contract — it is this frame's texture and it
		// stops being valid when the resource is resized or the pipeline
		// changes, so a caller reads it each frame rather than keeping it.
		//
		// **Only the ones that outlive a frame.** A scene target, the shadow
		// atlas, a surface, the overdraw counter and anything the graph
		// allocated all persist and can be shown; `window` cannot, because the
		// swapchain image belongs to the driver between frames.
		//
		// **Its own type rather than a `SceneExtent`.** That one describes a
		// scene slot, which is allocated in blocks and shows only its corner, so
		// it carries fractions and no size. A graph resource is exactly the size
		// it asked for and a panel needs to print that.
		//
		// @since v0.11
		struct ResourceImage {
			// A `void *` for `SceneTexture`'s reason: an `SDL_GPUTexture *` is
			// not a name this header may say.
			void *Texture = nullptr;

			uint32_t Width = 0;
			uint32_t Height = 0;

			bool IsValid() const {
				return Texture != nullptr && Width > 0 && Height > 0;
			}
		};

		// @param resource The name, as the graph gives it.
		// @return The texture and its size, or an invalid image when nothing of
		//         that name persists.
		ResourceImage ResourceTexture(const core::Name &resource) const;

		// Every resource name `ResourceTexture` would answer for, this frame.
		//
		// **For a panel building a list to click on**, so it offers what can be
		// shown rather than everything the graph names and then half of them
		// coming back empty.
		std::vector<core::Name> InspectableResources() const;

		// Reads one resource back each frame, for its histogram.
		//
		// **Separate from `ResourceTexture` because the two answer different
		// questions.** That one is *show me this*, and costs nothing — the panel
		// samples the texture the GPU already has. This one is *tell me what is
		// in it*, and costs a download, so it is one resource at a time and only
		// while somebody is looking.
		//
		// @param resource What to read back, or an invalid name to stop.
		void Inspect(const core::Name &resource);

		// What `Inspect` was last asked for.
		core::Name Inspecting() const;

		// Keeps a copy of what a scene slot currently holds, under a name.
		//
		// **A slot is scratch and this is how a picture outlives it.** There are
		// a handful of slots and they are drawn into on rotation, so whatever is
		// in one is gone within a few frames — which is why the studio's mesh
		// preview could only ever show the row under the cursor. A captured copy
		// is an ordinary entry in the texture table: `TextureHandle` returns it,
		// `DropTexture` releases it, and a list can draw a hundred of them.
		//
		// **The drawn rectangle, not the allocation.** A target is rounded up to
		// a block, so the copy is `SceneTextureExtent`'s rectangle and samples
		// whole — a consumer needs the handle and nothing else, which is the
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
		// is the one an interface recording a bind right now will sample — so
		// the two are read together and stay a matched pair through a resize.
		// On the frame a target is reallocated the interface binds the outgoing
		// texture, and this reports the outgoing texture's rectangle with it.
		//
		// While a panel is merely being dragged the allocation does not change,
		// so this trails the rectangle the frame is about to draw by one frame's
		// worth of drag — under a pixel of scale, against a whole stale frame
		// stretched to the wrong shape before targets were allocated in blocks.
		// See `SceneExtent`.
		//
		// @param slot Which offscreen target to ask about.
		// @return The drawn fraction, or the whole texture when that slot has
		//         never been drawn into.
		SceneExtent SceneTextureExtent(size_t slot = 0) const;

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
		// @param path Where to write a BMP. Empty cancels a pending request.
		void RequestSceneCapture(std::filesystem::path path);

		// The device and swapchain format a `FrameOverlayHook` builds against.
		//
		// @return The handles, both empty before Initialise.
		// Runs this pipeline instead of the standard frame.
		//
		// **The thing the whole node editor was for.** Until this existed,
		// editing a pipeline changed a document and not a frame — which is the
		// state `docs/PIPELINE_NODES.md` stage 3 called the one most likely to
		// be misread as the editor being broken. `graph::Execute` already
		// submits the frame; this is what decides *which* frame it submits.
		//
		// **Refused rather than half-applied.** A graph that does not compile,
		// or that names a node kind nothing can draw, leaves the previous
		// pipeline running and says why. A renderer that accepted it would
		// present a dark window and the scene would get the blame.
		//
		// @param pipeline What to run. Copied, because a caller editing a
		//                 document should not be editing the frame in flight.
		// @return Whether it was taken. On `false` the previous pipeline is
		//         still running and the reason is logged.
		bool SetPipeline(const graph::RenderGraph &pipeline);

		// The same, under a name a view can ask for.
		//
		// **This is what makes many cameras with many pipelines possible.** A
		// `View::Pipeline` naming one of these runs it; one naming nothing runs
		// the default. The frame's own block — the overlay and the editor's
		// chrome — always comes from the default, because a window has one of
		// those however many cameras drew into it.
		//
		// @param name     What to call it. An invalid name is refused.
		// @param pipeline What to run. Copied and compiled now, so a view naming
		//                 it later costs a lookup rather than a compile.
		// @return Whether it was taken. On `false` nothing was replaced and the
		//         reason is logged.
		bool SetPipeline(core::Name name, const graph::RenderGraph &pipeline);

		// Drops a named pipeline. Views naming it fall back to the default.
		bool RemovePipeline(core::Name name);

		// Every pipeline name this renderer can run.
		std::vector<core::Name> Pipelines() const;

		// Goes back to `graph::StandardGraph`, and forgets every named
		// pipeline.
		void ResetPipeline();

		// The debug download's most recent picture, and how old it is.
		//
		// **A frame late, and it says so.** A readback that waited would be a
		// stall in a panel; one that does not is a picture behind. The second is
		// fine and the staleness is reported rather than hidden — see
		// `render::PendingReadback`.
		//
		// Empty until a `viewer` node has run in a pipeline this renderer was
		// given, which the standard frame has none of.
		//
		// @since v0.11
		struct ReadbackImage {
			// Which resource it is of, and how big.
			//@{
			core::Name Source;
			uint32_t Width = 0;
			uint32_t Height = 0;
			//@}

			// The pixels, as the transfer buffer held them, and what they reduce
			// to.
			//@{
			std::span<const uint32_t> Pixels;
			ImageHistogram Histogram;
			//@}

			// How many frames ago this was asked for. Zero for no picture.
			uint64_t Age = 0;

			bool IsValid() const {
				return Width > 0 && Height > 0 && !Pixels.empty();
			}
		};

		// What the last `viewer` node produced.
		ReadbackImage Readback() const;

		BackendHandles Backend() const;

	  private:
		// Aborts when the caller is not the owning thread. See `IsOnOwningThread`.
		//
		// @param what The call being refused, for the message.
		void RequireOwningThread(const char *what) const;

		struct Impl;
		std::unique_ptr<Impl> State;

		// The thread that called `Initialise`, and the only one that may record.
		//
		// Not atomic, unlike `ecs::Store::Owner`, and the difference is the
		// point: a store's owner is written every tick by whichever worker
		// picked the world up, so that write races other threads' reads. This is
		// written by the constructor and again by `Initialise`, then only read —
		// so any thread that could observe a torn value is a thread already
		// violating the contract this exists to state.
		std::thread::id Owner;
	};
}
