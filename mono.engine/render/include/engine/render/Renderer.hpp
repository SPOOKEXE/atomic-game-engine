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
#include <engine/render/Flipbook.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/SurfaceCameras.hpp>

// `SurfaceView::Projection` is a matrix. glm has always arrived here through
// `core/types/CFrame.hpp` and `graph/Frustum.hpp`, but a header that names a
// type should say where it comes from rather than rely on a neighbour.
#include <glm/mat4x4.hpp>

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
	// Tests compare this order with graph::StandardPipeline.
	//
	// @since v0.6
	enum class Pass : uint8_t {
		// Depth from the light, over the whole scene rather than the culled set.
		Shadow,

		// The surface camera's view, into a texture a mirror samples.
		Surface,

		// The screen, with depth written.
		Opaque,

		// Blended, depth-tested, and not depth-written; shares Opaque's pass.
		Transparent,

		// The overlay texture, loaded over the frame rather than clearing it.
		Overlay,

		// Whatever a `FrameOverlayHook` records — the editor's chrome, and
		// nothing a game ships with.
		//
		// Last, so interface content stays above the world.
		//
		// @since v0.7
		Interface,

		// Not a pass. The count, for the bitmask below.
		Count,
	};

	// The names of those stages, in the same order.
	//
	// @return The six names, valid for the life of the program.
	std::span<const core::Name> PassOrder();

	// A second view, rendered into a texture instead of the swapchain.
	//
	// Surface textures are double-buffered, so what a pane shows the screen is
	// the frame before. **What a pane shows *another pane* is not**, and stopped
	// being so in v0.15: the levels below the first are rendered inside the frame
	// by a recursion, from cameras derived level by level. See `PaneNormal` for
	// what that needs and why iterating could not supply it.
	//
	// **Mirrors and cross-world windows, and since v0.15 nothing else.** A
	// same-world portal used to be one of these and is a `PortalView` now, for
	// the reason that type states in full: the two derive a sub-camera by
	// different rules, and only one of them is a reflection.
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

		// The pane this camera projects off, when it is a mirror parented to one.
		//
		// **What lets the surface pass descend, and the whole of what it was
		// missing.** `Frame` and `Projection` are a camera *already placed from
		// the eye*, so a pane appearing inside another pane's picture was drawn
		// and sampled from a viewpoint nobody was looking from — the coordinate
		// leaves the texture's rectangle and `opaque.frag` falls back to the flat
		// lit pane, which is what "a mirror in a mirror shows a blank slab" was.
		//
		// A mirror's camera is a function of the pane and the viewer —
		// `scene::ReflectCamera` — so a pass holding the *rectangle* can place
		// that camera for a viewer that is not the eye, which is exactly what the
		// level below a mirror needs. `PortalView` carries the same four vectors
		// for the same reason, and neither type reads them off a component,
		// because measuring a face is `scene::GatherSurfacePanes`' job.
		//
		// **A zero `PaneNormal` means "not a pane this pass may descend into"**,
		// and it is the ordinary case rather than an error: a camera parented to
		// the world has no face to reflect through, and one showing another
		// world's instances has no local geometry behind it. Both keep the single
		// eye-derived image they have always had.
		//
		// `Centre ± First ± Second` is the four corners, as everywhere else.
		//
		// @since v0.15
		//@{
		core::Vector3 PaneCentre;
		core::Vector3 PaneNormal;
		core::Vector3 PaneFirst;
		core::Vector3 PaneSecond;
		//@}

		// The lens the pane's author gave the camera, which is what a camera
		// placed for a deeper viewer has to be fitted at.
		//
		// **Carried rather than read back out of `Projection`.** The near plane is
		// recoverable from a projection matrix and the far plane is recoverable
		// badly, and both are already known where the pane was measured — so a
		// recursion that recovered them would be deriving, once per level, two
		// numbers that were handed over for free.
		//
		// @since v0.15
		//@{
		float PaneNear = 0.1f;
		float PaneFar = 500.0f;
		//@}

		// What it renders through, already built.
		//
		// **A projection handed in rather than a field of view derived here**,
		// and the change is what a portal rests on. A surface frustum is fitted
		// to a pane's four corners and is therefore **off-axis** — the four
		// edges lean independently, so a viewer standing to one side gets
		// exactly the pane instead of twice its width; and for a portal it is
		// also **obliquely clipped**, with the near plane skewed onto the
		// destination's plane so the wall the hole leads through cannot draw
		// across it. Neither is expressible as an angle and two distances,
		// which is what this field used to be.
		//
		// Built by `scene::SurfaceProjection` from the `scene::SurfaceLens` that
		// `scene::AimSurfaceCameras` fits, so the renderer neither measures a
		// pane nor knows what a portal is.
		//
		// @since v0.14
		glm::mat4 Projection{1.0f};

		// What moved the pane into the space this view was fitted to.
		//
		// **A pane samples its image by projecting its own world position, so a
		// camera that was fitted somewhere else needs the pane taken there
		// too.** The surface is rendered with the view's own matrix and read
		// back with that matrix times this one; `scene::SurfaceLens::Mapping` is
		// where the two are argued out.
		//
		// Identity is a mirror, and is what a host that has never heard of a
		// portal leaves it as — including one filling this struct by hand.
		//
		// @since v0.14
		glm::mat4 Mapping{1.0f};

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

		// What the image is put through before a pane shows it.
		//
		// **A grade at the sampling site rather than a second pass.** The
		// surface texture holds an ordinary picture of the world whatever this
		// says; `opaque.frag` applies the effect where the pane reads it. So an
		// effect costs no render target, no extra pipeline and no bind, and the
		// surfaces that ask for none pay for none.
		//
		// From `scene::SurfaceCamera::Effect`.
		//
		// @since v0.13
		scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

		// Which tags an instance must carry to be drawn into this surface, or
		// zero for all of them.
		//
		// From `scene::SurfaceCamera::TagFilter`. **Applied per instance in the
		// draw loop rather than by re-ordering the draw list**, because the
		// order is shared by every view and a filter is per view: partitioning
		// it for one surface would be partitioning it for all of them, and the
		// screen pass would then draw the group instead of the world.
		uint32_t TagFilter = 0;

		// How many times a second this surface may redraw, or zero for every
		// frame.
		//
		// From `scene::SurfaceCamera::FPS`, and honoured beside the content
		// signature rather than instead of it: a surface redraws when its image
		// has changed **and** it is visible **and** its interval has elapsed.
		// The three are independent reasons to skip, and a surface that fails
		// any of them keeps the texture it has along with the matrices that
		// drew it.
		//
		// **Needs `Renderer::SetAnimationTime`**, which is the frame clock this
		// class already has. A host that never sets one leaves it at zero, and
		// a surface capped against a clock that never advances would render once
		// and freeze — so a non-advancing clock is treated as uncapped.
		//
		// @since v0.15
		float FPS = 0.0f;

		// Which instances this surface draws, when they are not this world's.
		//
		// **What makes a portal able to show another world**, and it is a range
		// rather than a second draw list because the renderer uploads one
		// instance buffer per frame. A host that wants world B seen through
		// world A's pane puts B's instances in `Render`'s `foreign` argument and
		// names that range here; the surface pass then draws exactly it.
		//
		// **An offset into `foreign`, not into the world's own list**, and the
		// two are separate arguments for the reason that split exists: joined,
		// the far world would be culled, ordered and drawn as part of this one.
		// The renderer moves the range into buffer space itself, after the
		// ordering that would otherwise have invalidated it.
		//
		// **`Count == 0` means "this world", which is every mirror and every
		// same-world portal.** Those draw the `scene::ScenePlan`'s own runs —
		// the world minus the pane being rendered, plus every other pane — which
		// is the arrangement that makes a mirror of a mirror work and is not
		// something a foreign range can participate in: the plan describes this
		// world's partitioning and knows nothing about the appended tail.
		//
		// So a foreign surface is drawn **plainly and in one run**. A pane in
		// the far world shows as a flat pane rather than as a recursive image,
		// which is the same fallback a surface with no frame yet already gets
		// and is right for the same reason — a hole through a hole into a third
		// world is a feature nobody has asked for and would need a plan per
		// world per frame to serve.
		//
		// @since v0.14
		//@{
		uint32_t InstanceFirst = 0;
		uint32_t InstanceCount = 0;
		//@}
	};

	// One same-world portal, as the recursive pass needs it.
	//
	// **Beside `surfaces` and never one of them, because a hole and a mirror are
	// not the same map.** Both passes are recursions now and both derive each
	// level's camera from the level above — that much stopped being a difference
	// in v0.15, and the machinery for opening a pass and drawing the world into
	// it is shared between them. What is left is the rule itself, and it is not
	// close to the same: a mirror reflects the viewer through one plane, and a
	// hole carries it through `destination · half-turn · source⁻¹` onto a second
	// pane that may be turned, moved and resized anywhere at all.
	//
	// **And what the pane reads back differs with it.** A hole's sub-render is
	// the screen's own frustum skewed onto the far plane, so its pane samples by
	// screen position and lines up texel for texel. A mirror's is fitted to its
	// own rectangle, so its pane samples by projecting its world position through
	// the matrix that drew the level. Merging the two entries would mean one of
	// them carrying the other's fields empty.
	//
	// `NON-EUCLIDEAN.md`'s Part III is the whole argument for the hole, and
	// CodeParade's `Portal.cpp` is the model.
	//
	// **Same-world only.** A `scene::Portal` naming a `DestinationWorld` is a
	// window onto a second simulation rather than a hole in one space: it keeps
	// its `SurfaceView`, draws a foreign instance range, and does not recurse.
	//
	// @since v0.15
	struct PortalView {
		// Which surface slot the pane's draw instances carry, from
		// `scene::DrawInstance::Surface`.
		//
		// **How the pass finds the pane's geometry, and it draws no geometry of
		// its own.** The quad is the pane part already in the draw list, which
		// `scene::PartitionSurfaces` has already grouped into a run of its own —
		// so there is no second mesh, no vertex buffer, and nothing coplanar with
		// the pane to fight it for depth.
		int8_t Index = 0;

		// The slot of the hole at the far end of this one, or -1 for none.
		//
		// **Skipped by the level this one opens**, which is CodeParade's
		// `skipPortal` argument and is the same saving. A sub-camera stands at
		// the far pane looking away from it, so the far pane is at the level's
		// own clip plane: rendering through it produces a scene that is then
		// entirely clipped away, at the cost of a full sub-render. Cheaper to
		// name it than to draw it and throw it away.
		int8_t Partner = -1;

		// The pane's plane and rectangle in world space, exactly as
		// `scene::PortalSeam` states them: `Centre ± First ± Second` is the four
		// corners and `Normal` is the face's own normal.
		//
		// **The rectangle and not the pane's box**, because what has to be culled
		// per level is the hole rather than the slab it is cut into, and because
		// the sub-camera is derived by mapping *this* rectangle rather than
		// anything the draw list holds.
		//@{
		core::Vector3 Centre;
		core::Vector3 Normal;
		core::Vector3 First;
		core::Vector3 Second;
		//@}

		// The map to the far side.
		//
		// **One map and not one per side, which is the point.** It carries this
		// pane's front hemisphere to the far pane's back one and its back to the
		// far pane's front, so it is right at the top level and right at every
		// level below where the sub-camera has stepped through — and it is the
		// exact map `scene::CrossPortals` moves a body by, so what the hole shows
		// and where walking into it puts you cannot come apart. CodeParade's
		// `Portal::Warp::delta`, which is likewise one matrix for both sides.
		scene::SeamTransform Warp;

		// Which tags an instance must carry to be drawn through this hole, or
		// zero for all of them. `SurfaceView::TagFilter`'s argument, unchanged.
		uint32_t TagFilter = 0;
	};

	// How many levels of portal recursion a renderer will go to.
	//
	// **A ceiling rather than a budget**, because the pool is allocated per level
	// per slot at viewport resolution: four levels of a sixteen-slot scene would
	// be sixty-four full-screen colour and depth pairs, which is video memory
	// nobody asked for. Anything a caller asks for above this is clamped, with
	// the frame drawn rather than refused.
	//
	// @since v0.15
	inline constexpr uint32_t MAX_PORTAL_DEPTH = 4;

	// How many levels of mirror-in-mirror a renderer will resolve.
	//
	// **A ceiling for the same reason `MAX_PORTAL_DEPTH` is one, and it arrived
	// with the same change.** While the surface pass resolved a chain by running
	// itself again, depth cost one extra pass per visible pane per level and an
	// ambitious number merely wasted time. It is a recursion now — each level
	// descends into every *other* pane it can see — so the passes go as
	// `panes × (panes - 1) ^ (depth - 1)` before the per-level visibility test
	// takes them back down, and a scene that asked for eight would ask the device
	// for something it cannot finish inside a frame.
	//
	// Three rather than four, because a mirror is not a hole: what you see at
	// the third bounce is a few pixels of a few pixels and the fourth is not
	// distinguishable from the ambient it fades into, whereas a corridor of
	// portals is the whole subject of the scene it appears in.
	//
	// Anything above this is clamped, with the frame drawn rather than refused.
	//
	// **It is also what the automatic depth is bounded by**, and that is the
	// half worth stating: `SetSurfaceBounces(0)` lets each viewport measure its
	// own depth from the frame it just drew, and a corridor of facing panes says
	// "one deeper" at every level for ever. This is what stops it, and it is why
	// `scene::NextSurfaceBounces` takes a ceiling rather than knowing one.
	//
	// @since v0.15
	inline constexpr uint32_t MAX_SURFACE_DEPTH = 3;

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

		// How many portal sub-renders this frame ran.
		//
		// **Reported separately from `SurfacePasses` because it is the number
		// that goes up fastest.** A level is a whole scene rendered from a
		// camera that did not exist a moment ago, and the count is holes times
		// levels in the worst case — so this is where a corridor of portals
		// shows up, and where `graph::VisiblePane` doing its job shows up as the
		// same corridor costing two.
		//
		// @since v0.15
		uint32_t PortalPasses = 0;

		// How many ribbon vertices were submitted this frame.
		//
		// Two per segment, so a beam is twenty-two and a trail is at most
		// thirty-two. Reported beside the particle count because the two are the
		// same question asked of the other half of the module.
		//
		// @since v0.10
		uint32_t RibbonVertices = 0;

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

		// Whether one pass submitted work this frame.
		//
		// @param pass Which one.
		// @return True when it ran.
		bool Ran(Pass pass) const {
			return (Passes & static_cast<uint8_t>(1u << static_cast<uint8_t>(pass))) != 0;
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
		// **How many frames the CPU may queue ahead of the GPU is a policy, and
		// the caller owns it.** One means `SDL_SubmitGPUCommandBuffer` blocks
		// until the GPU has finished the previous frame — the CPU and the GPU
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
		// @return True when the device, pipelines and geometry are ready.
		bool Initialise(SDL_Window *window, uint32_t framesInFlight = 1);

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

		// Says that content is on its way under this name, and that it is not.
		//
		// **The fact the renderer cannot learn for itself.** It knows what it
		// holds; what is in flight belongs to the content pump, which is a layer
		// above. Between these two calls a drawable naming that texture draws as
		// the default material rather than as the purple marker — so a scene
		// load looks like untextured parts becoming textured, instead of a
		// purple shimmer indistinguishable from forty misspellings.
		//
		// **`StopExpecting` goes on the request *finishing*, not on it
		// succeeding.** A host that unmarks only on arrival leaves a misspelled
		// name expected for ever, which is exactly the case the marker exists
		// for. An arrival needs no call at all: `AddTexture` clears it.
		//
		// @param name What was asked for.
		// @since v0.13
		//@{
		void ExpectTexture(const core::Name &name);
		void StopExpectingTexture(const core::Name &name);
		//@}

		// Whether content is on its way under this name.
		//
		// @param name The name.
		// @return `true` between the two calls above.
		// @since v0.13
		bool ExpectingTexture(const core::Name &name) const;

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
		// staleness `D00112` was about and could never fix the viewpoint —
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
		// them back down. That is why there is a ceiling now — see
		// `MAX_SURFACE_DEPTH`.
		//
		// **Zero is `scene::AUTOMATIC_SURFACE_BOUNCES` and is the default.** It
		// used to mean "no surface pass at all", which nobody ever wanted and
		// which has a clearer spelling. What it means now is that each viewport
		// measures the frame it just drew — how deep the chain of panes actually
		// went, and whether the deepest level still had a pane in view it was not
		// allowed to descend into — and draws one level deeper next frame when
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
		// resolves whatever chain it has by iterating —
		// `scene::DEFAULT_SURFACE_BOUNCES` of them, since there is nothing there
		// to measure. See `SurfaceView::PaneNormal`.
		//
		// @param bounces How many levels to resolve, or zero to measure it.
		// @since v0.15
		void SetSurfaceBounces(uint32_t bounces);

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
		// above — so within one command buffer the same texture is written,
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
		// drawn with — so a host that does not care is not made to care.
		//
		// **The direction is what the shadow map is fitted along as well as what
		// the shader shades with**, so setting it moves both together. That is
		// the whole reason it is one call.
		//
		// @param direction Where it shines towards. Normalised here; a zero
		//                  vector is ignored rather than obeyed.
		// @param ambient   What reaches a surface the sun does not.
		// @since v0.15
		void SetSun(const core::Vector3 &direction, const core::Color3 &ambient);

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

		// Registers a fragment shader under a name a draw instance can select.
		//
		// **The words are already SPIR-V.** Where they came from is the caller's
		// business and there are two answers: `render::ShaderLibrary` compiled a
		// `ShaderScript`'s GLSL, or it read a built-in `glslc` produced during
		// the build. Neither is this class's concern — it builds the pipelines a
		// `scene::DrawInstance::Shader` naming this resolves to.
		//
		// **The module must declare what `opaque.frag` declares**: four fragment
		// samplers and three uniform buffers, in those slots. A shader object
		// carries those counts rather than the pipeline doing so, so a module
		// that declares different ones binds and silently samples nothing — the
		// same trap the built-in pipeline's own comments record. That interface
		// is what a `ShaderScript` is written against.
		//
		// **Only the fragment stage, deliberately.** The vertex stage owns the
		// instance layout, which `AGENTS.md` says is private and stays private —
		// a caller able to replace it would be authoring against a struct nobody
		// promised to keep.
		//
		// **Two pipelines are built, opaque and transparent**, because blend
		// state is baked into a pipeline. The shadow pass draws through neither:
		// it writes depth and no colour, so a fragment shader there would cost a
		// pass over the scene to produce nothing.
		//
		// Registering a name twice replaces it, which is what an author editing
		// a shader does; the frame in flight is waited for before the old
		// pipeline is released.
		//
		// @param name  What a material names it.
		// @param spirv The module's words. Copied into device objects; not retained.
		// @return `false` when the module or either pipeline could not be built.
		//         The error is logged with the shader's name.
		// @since v0.15
		bool AddShader(const core::Name &name, std::span<const uint32_t> spirv);

		// Forgets a registered shader and frees its pipelines.
		//
		// Instances still naming it fall back to the engine's own shader rather
		// than vanishing — `MeshTable::Resolve`'s rule, and its reason.
		//
		// @param name The name to drop.
		// @return `false` for a name this renderer does not hold.
		// @since v0.15
		bool DropShader(const core::Name &name);

		// Whether a shader is registered under this name.
		//
		// @param name The name.
		// @return `true` when a variant exists for it.
		// @since v0.15
		bool HasShader(const core::Name &name) const;

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

		// Draws one frame and presents it. Returns false in Presented when the
		// swapchain had no texture — minimised, or resizing — which is not an
		// error and not a reason to stop ticking. It is also false if SDL rejects
		// command submission.
		//
		// `overlay` is uploaded only when it has something pending, and drawn
		// whenever it has content — the texture holds the last thing sent to it,
		// so a caller may redraw the overlay far less often than it presents.
		// The renderer marks the overlay uploaded once it has recorded the copy,
		// which is the only reason it is not a const reference; nothing else
		// about it is retained past the call.
		//
		// The camera and instances are copied during the call and not retained.
		//
		// The aspect ratio comes from the swapchain texture this call acquired
		// rather than from a caller, so a frame taken during a resize is
		// projected for the image it is actually drawn into. `scene::Camera`
		// therefore holds no aspect ratio and `scene::ResolveCamera` takes one.
		//
		// @param cameraFrame World-space placement of the camera.
		// @param camera      Field of view and clipping distances.
		// @param instances   What to draw, as the world described it. Each is
		//                    turned into a model matrix and a colour here.
		// @param overlay     CPU premultiplied RGBA8 overlay. Uploaded only when
		//                    it has a pending region, drawn whenever it has
		//                    content, and marked uploaded on the way out.
		// @param surfaces    The offscreen views to render first, one per surface
		//                    index. Empty for a scene with no mirror in it.
		// @param interfaceHook An editor's chrome, drawn last, or null. See
		//                    `FrameOverlayHook` for why this is not imgui.
		// @param sceneTarget Draw the world into an offscreen texture of this
		//                    size instead of into the window, or null for the
		//                    window. Decides the aspect ratio and the cull
		//                    frustum as well as the target — see `SceneTarget`.
		// @param targetSlot  Which viewport this call is drawing. A game draws
		//                    one view and never passes this; a studio keeps a
		//                    slot per viewport so two panels of different sizes
		//                    do not reallocate one shared texture twice a frame.
		//                    See `SceneTexture`.
		//
		//                    Selects the surface texture bank for this viewport.
		// @param particles  One batch per emitter with live particles, drawn after
		//                    the blended geometry and before the overlay. Empty
		//                    for a scene with no effects in it, which is every
		//                    scene that has not installed a particle pool.
		//
		//                    **After the blended pass and never sorted against
		//                    it**, which is the same trade `ScenePlan::
		//                    TransparentSurfaces` makes for mirrors: a particle in
		//                    front of a pane of glass is drawn after it whatever
		//                    the depths say. One sorted run per pipeline is what
		//                    lets the blend mode be a pipeline binding instead of
		//                    a per-fragment branch, and interleaving half a million
		//                    particles into the geometry sort would cost more than
		//                    the artefact does.
		// @param ribbonVertices Every beam and trail vertex this world produced,
		//                    as `effects::BuildRibbons` packed them. Passed as the
		//                    whole stream rather than per run, because the runs
		//                    index into it.
		// @param ribbonRuns  Where each ribbon sits in that stream.
		//
		//                    **Drawn after the particles**, which is one more
		//                    fixed ordering in a pass that has several — see the
		//                    `particles` note above. A beam is usually the thing
		//                    an author wants on top of its own sparks, so the
		//                    order is the useful one rather than an accident, and
		//                    it is fixed rather than sorted for the same reason
		//                    every other run here is.
		// @param lights     The point and spot lights near this view, at most
		//                    `MAX_SCENE_LIGHTS`. **Added to the directional term
		//                    rather than replacing it**, so a scene with no lamps
		//                    looks exactly as it did before v0.10 — which is what
		//                    makes this safe to switch on for every existing world.
		//
		//                    Anything past the cap is dropped, and the caller is
		//                    the one that should be choosing which: the renderer
		//                    has no idea which lamp matters. `client::CollectLights`
		//                    picks the nearest to the eye.
		// @param foreign    Instances belonging to *other* worlds, drawn only
		//                    through the surfaces that name them by
		//                    `SurfaceView::InstanceFirst`.
		//
		//                    **Separate from `instances` because everything this
		//                    call does to `instances` is wrong for these.** They
		//                    are not culled against this camera, not sorted into
		//                    this view's plan, and never submitted by the screen
		//                    pass or the shadow pass — a second world is not
		//                    part of this world's frame, it is a picture hanging
		//                    in it. They were appended to `instances` by the host
		//                    until v0.14, and the two rooms drew on top of each
		//                    other.
		//
		//                    They still share the one instance buffer: the
		//                    renderer copies them in after this world's rows and
		//                    shifts each surface's range to match.
		// @param portals    The same-world holes, drawn by the recursive pass
		//                    instead of by a surface camera.
		//
		//                    **Beside `surfaces` rather than among them**, which
		//                    is the whole of `PortalView`: a surface's camera is
		//                    a function of the eye and cannot compose down a
		//                    recursion, and a portal's has to. A slot named here
		//                    must not also be named in `surfaces` — the pane
		//                    would be drawn twice, once by each pass, and the
		//                    renderer drops the surface with a line in the log
		//                    rather than choosing silently.
		// @return Submitted draw counts and whether the frame was presented.
		FrameResult Render(
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			std::span<const scene::DrawInstance> instances,
			OverlayImage &overlay,
			std::span<const SurfaceView> surfaces = {},
			FrameOverlayHook *interfaceHook = nullptr,
			const SceneTarget *sceneTarget = nullptr,
			size_t targetSlot = 0,
			std::span<const ParticleBatch> particles = {},
			std::span<const effects::RibbonVertex> ribbonVertices = {},
			std::span<const effects::RibbonRun> ribbonRuns = {},
			std::span<const SceneLight> lights = {},
			std::span<const scene::DrawInstance> foreign = {},
			std::span<const PortalView> portals = {}
		);

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
		// **Which viewport, because a host with two draws them in turn.** The
		// request is made after one panel's `Render` returns and consumed by the
		// *next* one, so a caller that asked while panel A was showing got
		// panel B's picture — and with a scene per panel that is a photograph of
		// a world nobody named. Naming the slot makes the request wait for that
		// panel's turn instead of taking whichever came next.
		//
		// @param path Where to write a BMP. Empty cancels a pending request.
		// @param slot Which viewport's scene to photograph, or `ANY_VIEWPORT`
		//             for whichever draws next — which is the old behaviour and
		//             is right for a host with one panel.
		void RequestSceneCapture(std::filesystem::path path, size_t slot = ANY_VIEWPORT);

		// Any viewport will do. See `RequestSceneCapture`.
		//
		// @since v0.15
		static constexpr size_t ANY_VIEWPORT = static_cast<size_t>(-1);

		// The device and swapchain format a `FrameOverlayHook` builds against.
		//
		// @return The handles, both empty before Initialise.
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
